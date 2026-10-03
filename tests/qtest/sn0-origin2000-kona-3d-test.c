/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA fixed-function 3D qtests.
 *
 * Covers the GL client's 3D commands on the command page: that every
 * command of the 3D grammar frames with its word count, the packed
 * attribute forms included, and that the GL commands sharing the PROM
 * textport's numbers stay GL commands once a context is active.
 *
 * The fixture is KONA as a -device at the empty slot IO4, crossbow0's
 * widget 11, with fabric-hub-widget=9 so the board's writes reach node
 * 0's memory, on a machine with one node board.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qobject/qdict.h"
#include <math.h>
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

#define INT_CAUSE_MAILBOX   0x10

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4,fabric-hub-widget=9"

/* Node 0 guest RAM: the GL context's save area and return page. */
#define SAVE_AREA   UINT64_C(0x340000)
#define RETURN_PAGE UINT64_C(0x320000)

/* IEEE single-precision 0.0 and 1.0, as the client stores them. */
#define F0 0x00000000u
#define F1 0x3f800000u

/*
 * A fresh instance tracing every sgi_kona event and logging guest errors
 * and unimplemented accesses to its own file, with extra arguments. The
 * caller quits it and reads the log with kona_log_read(), which also
 * removes the file.
 */
static QTestState *kona_init_logged_args(char **log, const char *extra)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-3d-XXXXXX.log", g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    return qtest_initf("%s -trace enable=sgi_kona_* -d guest_errors,unimp"
                       " -D %s%s", MACHINE_ARGS, *log, extra);
}

static QTestState *kona_init_logged(char **log)
{
    return kona_init_logged_args(log, "");
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

/*
 * True if the log shows a word taken off the stream as unframed. The
 * shared GL layer's own "sgi-gl:" messages are about the state the
 * words set, not their framing.
 */
static bool log_shows_misframing(const char *found)
{
    g_auto(GStrv) lines = g_strsplit(found, "\n", -1);

    for (char **l = lines; *l; l++) {
        if (g_str_has_prefix(*l, "sgi-gl:")) {
            continue;
        }
        if (strstr(*l, "dropped") ||
            strstr(*l, "textport expected a sync word") ||
            strstr(*l, "unsupported textport selector") ||
            strstr(*l, "not framed")) {
            return true;
        }
    }
    return false;
}

/* One command on the +0xc10000 port, as the GL client's doorbell sends. */
static void kona_command(QTestState *s, uint32_t token, const uint32_t *args,
                         unsigned argc)
{
    qtest_writel(s, REG_COMMAND, token);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
}

/* The kernel's bracket: kick 0x30000, token, kick 0x10000, words, kick 0. */
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

/* A 0x1f2 or 0x1fd page word and tagged offset word for a physical page. */
static uint32_t physical_page_word(uint64_t addr)
{
    return 0x10000000 | (uint32_t)(addr >> 12);
}

static uint32_t tagged_offset_word(uint64_t addr)
{
    return 0x9000 | (uint32_t)(addr & 0xfff);
}

/*
 * The GL context's first make-current: KONApcxswap's 0x1fd and 0x1f2,
 * then the client's buffer configuration, single-buffered with 12-bit
 * components and `depth` depth bits, and the pack word. Four commands.
 */
static void gl_make_current_depth(QTestState *s, uint32_t depth)
{
    uint32_t config[10] = { 0, 0xc, 0, 0, 0, 0, depth };

    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ physical_page_word(SAVE_AREA),
                                      tagged_offset_word(SAVE_AREA),
                                      0x10000 }, 3);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    kona_command(s, 1, config, 10);
    kona_command(s, 2, (uint32_t[]){ 0 }, 1);
}

static void gl_make_current(QTestState *s)
{
    gl_make_current_depth(s, 0);
}

/* The commands gl_make_current() frames, for the trace count. */
#define MAKE_CURRENT_COMMANDS 4

/*
 * The GL commands with a fixed count, the words after the selector word
 * with a double counting two, as the senders write them. Written out
 * again here rather than read from the model. The buffer configuration
 * and the commands with data runs are exercised separately.
 */
static const struct {
    uint32_t selector;
    uint32_t words;
} gl_commands[] = {
    { 0x2, 1 }, { 0x4, 10 }, { 0x114, 1 },
    { 0x177, 1 }, { 0x1d7, 4 }, { 0xee, 1 }, { 0x1ce, 1 }, { 0xe3, 4 },
    { 0xc, 4 }, { 0xe4, 1 },
    { 0x1d, 4 }, { 0x1a, 1 }, { 0x1b, 4 }, { 0x1c, 1 }, { 0x1e, 1 },
    { 0x1f, 2 }, { 0x57, 1 }, { 0x58, 1 }, { 0x59, 1 }, { 0x5a, 1 },
    { 0x5b, 1 }, { 0x5c, 1 }, { 0x5d, 1 }, { 0x5e, 1 }, { 0x5f, 1 },
    { 0x60, 1 }, { 0x71, 1 }, { 0x3, 1 }, { 0xc7, 1 }, { 0xc6, 1 },
    { 0x1cc, 1 }, { 0x3e, 1 }, { 0x1ca, 1 }, { 0x1d6, 1 },
    { 0xcf, 4 }, { 0xd0, 1 }, { 0xd1, 12 }, { 0xd2, 1 }, { 0xd3, 16 },
    { 0xd4, 32 }, { 0xd5, 16 }, { 0xd6, 32 }, { 0xd7, 12 }, { 0xd8, 1 },
    { 0xd9, 1 }, { 0xda, 8 }, { 0xdb, 4 }, { 0xdc, 6 }, { 0xdd, 3 },
    { 0xde, 6 }, { 0xdf, 3 },
    { 0x62, 6 }, { 0x63, 3 }, { 0x64, 3 }, { 0x67, 3 }, { 0x6a, 8 },
    { 0x6b, 4 }, { 0x6c, 4 }, { 0x6f, 4 }, { 0x77, 6 }, { 0x78, 3 },
    { 0x79, 3 }, { 0x7c, 1 }, { 0x80, 2 }, { 0x84, 3 }, { 0x88, 4 },
    { 0x8b, 4 }, { 0x8c, 2 }, { 0x8d, 2 }, { 0x8f, 6 }, { 0x90, 3 },
    { 0x91, 3 }, { 0x93, 8 }, { 0x94, 4 }, { 0x95, 4 }, { 0xa7, 1 },
    { 0xa9, 2 }, { 0xad, 3 }, { 0xb1, 4 },
    { 0x7, 6 }, { 0x8, 6 }, { 0x9, 5 }, { 0xa, 5 }, { 0x97, 6 },
    { 0xb5, 2 },
    { 0x24, 1 }, { 0x25, 1 }, { 0xc0, 1 }, { 0xce, 1 }, { 0x22, 1 },
    { 0xcb, 2 }, { 0xe6, 4 }, { 0xe7, 1 }, { 0xca, 2 }, { 0xcc, 3 },
    { 0xcd, 3 }, { 0x20, 1 }, { 0x21, 4 }, { 0x23, 1 }, { 0x2e, 1 },
    { 0xb6, 1 }, { 0xba, 1 }, { 0xbf, 2 }, { 0x168, 2 }, { 0x11b, 2 },
    { 0xbd, 1 }, { 0xbe, 1 }, { 0xbc, 2 }, { 0xb7, 5 }, { 0xb8, 5 },
    { 0xb4, 9 }, { 0xbb, 2 }, { 0xc4, 1 }, { 0x37, 1 }, { 0x33, 2 },
    { 0x11, 5 }, { 0x12, 5 },
    { 0x3a, 8 }, { 0x39, 7 }, { 0x4c, 1 }, { 0xc9, 1 }, { 0xc8, 1 },
    { 0x176, 1 }, { 0x178, 1 }, { 0x179, 22 }, { 0x1d5, 5 },
    { 0x106, 5 }, { 0xed, 4 },
};

/*
 * Words that each misframe the stream if a count is wrong: a word left
 * over is dropped or opens a command of its own, such as the reason post
 * 0x1f8 or the GL vertex 0x8c, and one taken too many swallows the next
 * command's selector, whose trace line then goes missing.
 */
static const uint32_t hostile[] = {
    0x9, 0x1f8, 0x0, 0x3e, 0xa, 0x103, 0x8c, 0x24, 0xd9, 0x0,
};

static uint32_t hostile_word(uint32_t i)
{
    return hostile[i % ARRAY_SIZE(hostile)];
}

/* The trace line of a framed command whose words are hostile_word(seed). */
static char *framed_line(uint32_t selector, uint32_t words, uint32_t seed)
{
    return g_strdup_printf("token 0x%x args 0x%x 0x%x 0x%x argc %u\n",
                           selector, hostile_word(seed),
                           words > 1 ? hostile_word(seed + 1) : 0,
                           words > 2 ? hostile_word(seed + 2) : 0, words);
}

static void send_hostile(QTestState *s, uint32_t selector, uint32_t words,
                         uint32_t seed)
{
    qtest_writel(s, REG_COMMAND, selector);
    for (uint32_t i = 0; i < words; i++) {
        qtest_writel(s, REG_COMMAND, hostile_word(seed + i));
    }
}

/*
 * After a context's first make-current, every GL command of the 3D
 * grammar with a fixed count frames whole, each with words that misframe
 * the stream on a wrong count; then the four with data runs: the
 * texture table's 33 words, the polygon stipple's 32 rows, the pop-lights
 * count and its 24 words, and a 3x2 RGBA texel load from a source two
 * bytes into a word, whose rows are two words of padding and pixels each.
 * No word is dropped, every command's trace line is there, the count of
 * framed commands is exact, and the textport answers its sync probe
 * afterwards.
 */
static void test_every_command_frames(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t header[20];
    unsigned commands = MAKE_CURRENT_COMMANDS;

    gl_make_current(s);
    for (size_t i = 0; i < ARRAY_SIZE(gl_commands); i++) {
        send_hostile(s, gl_commands[i].selector, gl_commands[i].words, i);
        commands++;
    }
    send_hostile(s, 0x17e, 1 + 33, 3);
    send_hostile(s, 0xb, 3 + 32, 5);
    kona_command(s, 0x119, (uint32_t[]){ 24 }, 1);
    for (uint32_t i = 0; i < 24; i++) {
        qtest_writel(s, REG_COMMAND, hostile_word(i));
    }
    for (uint32_t i = 0; i < 20; i++) {
        header[i] = hostile_word(i);
    }
    header[1] = 3;
    header[2] = 2;
    header[3] = 0x1908;
    header[4] = 0x1401;
    header[5] = 2;
    header[6] = 0;
    kona_command(s, 0x174, header, 20);
    for (uint32_t i = 0; i < 2 * 4; i++) {
        qtest_writel(s, REG_COMMAND, hostile_word(i + 4));
    }
    kona_command(s, 0x176, (uint32_t[]){ 0x8c }, 1);
    commands += 5;
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    for (size_t i = 0; i < ARRAY_SIZE(gl_commands); i++) {
        g_autofree char *line = framed_line(gl_commands[i].selector,
                                            gl_commands[i].words, i);

        g_assert_nonnull(strstr(found, line));
    }
    g_assert_nonnull(strstr(found, "token 0xb args 0x103 0x8c 0x24 argc 3\n"));
    g_assert_nonnull(strstr(found, "token 0x119 args 0x18 0x0 0x0 argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x174 args 0x9 0x3 0x2 argc 20\n"));
    g_assert_nonnull(strstr(found, "sgi_kona_gl_data token 0xb data words"
                                   " 32\n"));
    g_assert_nonnull(strstr(found, "sgi_kona_gl_data token 0x119 data words"
                                   " 24\n"));
    g_assert_nonnull(strstr(found, "sgi_kona_gl_data token 0x174 data words"
                                   " 8\n"));
    g_assert_cmpuint(count_occurrences(found, "token 0x17e argc 34 consumed"),
                     ==, 1);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_kernel_command "),
                     ==, commands);
}

/*
 * The byte and short attribute forms carry data in the top half of the
 * selector word: glColor3ub(0x12, 0x34, 1) is 0x12340066 then 0x01000000,
 * and the same holds for the other packed forms. Each frames as its
 * selector with its count, and its second word, which has no GL selector
 * in its low half, is not dropped.
 */
static void test_packed_forms_frame(void)
{
    static const struct {
        uint32_t words[3];
        uint32_t n;
    } packed[] = {
        { { 0x12340066, 0x01000000 }, 2 },             /* glColor3ub */
        { { 0x80ff0061, 0x7f000000 }, 2 },             /* glColor3b */
        { { 0x1020006e, 0x30400000 }, 2 },             /* glColor4ub */
        { { 0x7fff006d, 0x00017ffe, 0x80000000 }, 3 }, /* glColor4s */
        { { 0x7f810076, 0x01000000 }, 2 },             /* glNormal3b */
        { { 0x0001007a, 0xfffe0003 }, 2 },             /* glNormal3s */
        { { 0x00050082, 0x00060000 }, 2 },             /* glTexCoord2s */
        { { 0x0003008e, 0xfffe0000 }, 2 },             /* glVertex2s */
        { { 0x00040092, 0x00050006 }, 2 },             /* glVertex3s */
        { { 0x00070096, 0x00080009, 0x00010000 }, 3 }, /* glVertex4s */
    };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    for (size_t i = 0; i < ARRAY_SIZE(packed); i++) {
        for (uint32_t w = 0; w < packed[i].n; w++) {
            qtest_writel(s, REG_COMMAND, packed[i].words[w]);
        }
    }
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    for (size_t i = 0; i < ARRAY_SIZE(packed); i++) {
        g_autofree char *line = g_strdup_printf(
            "token 0x%x args 0x%x 0x%x 0x0 argc %u\n",
            packed[i].words[0] & 0xffff, packed[i].words[1],
            packed[i].n > 2 ? packed[i].words[2] : 0, packed[i].n - 1);
        g_autofree char *data = g_strdup_printf(
            "selector 0x%x data 0x%x\n", packed[i].words[0] & 0xffff,
            packed[i].words[0] & 0xffff0000);

        g_assert_nonnull(strstr(found, line));
        g_assert_nonnull(strstr(found, data));
    }
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_kernel_command "),
                     ==, MAKE_CURRENT_COMMANDS + ARRAY_SIZE(packed));
}

/*
 * The textport's bitmap blit as the PROM sends it: the sync word 1, the
 * command 0xa, the six header words of an 8x1 bitmap and its one payload
 * word.
 */
static void textport_bitmap(QTestState *s)
{
    static const uint32_t words[] = {
        1, 0xa, 8, 1, 0, 0, 8, 0, 0xff000000,
    };

    for (size_t i = 0; i < ARRAY_SIZE(words); i++) {
        qtest_writel(s, REG_COMMAND, words[i]);
    }
}

/*
 * glLightModeli shares 0xa with the textport's bitmap blit, and glLight
 * and glLightModelf 7 and 9 with two textport commands. Before any
 * context activation the page speaks the textport grammar: the PROM's
 * bitmap blit draws, and a bare 7 is a stray word, not glLight. After a
 * context's first make-current a bare 0xa is glLightModeli with its five
 * words, as are 7 and 9 with theirs, and none of their words starts a
 * blit; a bitmap blit after the sync word still draws.
 */
static void test_light_model_not_bitmap(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *before = NULL;
    QTestState *s = kona_init_logged(&log);

    textport_bitmap(s);
    qtest_writel(s, REG_COMMAND, 7);
    assert_textport_in_step(s);
    qtest_quit(s);
    before = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(before, "sgi_kona_bitmap_blit "), ==,
                     1);
    g_assert_nonnull(strstr(before, "textport expected a sync word, got"
                                    " 0x7\n"));
    g_free(log);

    s = kona_init_logged(&log);
    gl_make_current(s);
    kona_command(s, 0xa, (uint32_t[]){ 0xb52, 1, 0, 0, 0 }, 5);
    kona_command(s, 0x9, (uint32_t[]){ 0xb53, F1, F0, F1, F1 }, 5);
    kona_command(s, 0x7, (uint32_t[]){ 0x4000, 0x1203, F0, F0, F1, F0 }, 6);
    kona_command(s, 0xa, (uint32_t[]){ 0xb51, 8, 1, 0, 0 }, 5);
    textport_bitmap(s);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0xa args 0xb52 0x1 0x0 argc 5\n"));
    g_assert_nonnull(strstr(found, "token 0x9 args 0xb53 0x3f800000 0x0"
                                   " argc 5\n"));
    g_assert_nonnull(strstr(found, "token 0x7 args 0x4000 0x1203 0x0"
                                   " argc 6\n"));
    g_assert_nonnull(strstr(found, "token 0xa args 0xb51 0x8 0x1 argc 5\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_bitmap_blit "), ==,
                     1);
}

/*
 * A GL data run caught by a migration finishes on the destination:
 * glPopAttrib's pop-lights with 24 words, ten sent before the migration
 * and fourteen after, the first three of them the sync probe 1 9 0. A
 * destination that lost the run answers that probe and drops the words
 * after it; one that kept it frames the packed glColor3ub after the run
 * and answers the probe that follows.
 */
static void test_migration_mid_data_run(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-3d-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(" -incoming %s", uri);
    to = kona_init_logged_args(&log, dest_args);

    gl_make_current(from);
    kona_command(from, 0x119, (uint32_t[]){ 24 }, 1);
    for (uint32_t i = 0; i < 10; i++) {
        qtest_writel(from, REG_COMMAND, hostile_word(i));
    }
    sgi_migrate_stopped(from, uri);

    qtest_writel(to, REG_COMMAND, 1);
    qtest_writel(to, REG_COMMAND, 9);
    qtest_writel(to, REG_COMMAND, 0);
    for (uint32_t i = 3; i < 14; i++) {
        qtest_writel(to, REG_COMMAND, hostile_word(i));
    }
    g_assert_cmphex(qtest_readl(to, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==, 0);
    qtest_writel(to, REG_COMMAND, 0x12340066);
    qtest_writel(to, REG_COMMAND, 0x56000000);
    assert_textport_in_step(to);
    qtest_quit(to);
    qtest_quit(from);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0x119 argc 24 consumed"));
    g_assert_nonnull(strstr(found, "token 0x66 args 0x56000000 0x0 0x0"
                                   " argc 1\n"));
    g_rmdir(tmpfs);
}

/* The probe's window: 128 pixels square, lower-left pixel (256, 640). */
#define PROBE_SIZE  128
#define PROBE_X     256
#define PROBE_Y     640
#define FB_WIDTH    1280
#define FB_HEIGHT   1024

/*
 * A pixel centre must lie at least this far, in pixels, from an edge that
 * could decide it, so neither a tie rule nor snapping the vertices to a
 * grid of 2^-8 pixel or finer, which moves an edge by at most 2^-9, can
 * change the expected image.
 */
#define EDGE_MARGIN (1.0 / 256)

static QTestState *qts;

/* A clean shared machine for each drawing case. */
static void gl_reset(void)
{
    qtest_system_reset(qts);
    qtest_memset(qts, RETURN_PAGE, 0, 0x4000);
}

/* A single float's bits, and a double as the two words it reaches. */
static uint32_t fbits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static void dwords(double d, uint32_t *w)
{
    uint64_t bits;

    memcpy(&bits, &d, sizeof(bits));
    w[0] = bits >> 32;
    w[1] = (uint32_t)bits;
}

/* A command whose arguments are all doubles, high word first. */
static void kona_command_d(QTestState *s, uint32_t token, const double *v,
                           unsigned n)
{
    uint32_t w[32];

    for (unsigned i = 0; i < n; i++) {
        dwords(v[i], &w[2 * i]);
    }
    kona_command(s, token, w, 2 * n);
}

/* A command whose arguments are all floats. */
static void kona_command_f(QTestState *s, uint32_t token, const float *v,
                           unsigned n)
{
    uint32_t w[16];

    for (unsigned i = 0; i < n; i++) {
        w[i] = fbits(v[i]);
    }
    kona_command(s, token, w, n);
}

/*
 * kona_act_pcx's 0x1f3 and 0x1ee for the probe's window, as the GL suite
 * sends them, then the client's viewport, flat shading and a clear in
 * the colour r, g, b.
 */
static void gl_window_cleared(QTestState *s, float r, float g, float b)
{
    kona_command_kicked(s, 0x1f3,
                        (uint32_t[]){ PROBE_X, PROBE_Y, 0x05670080,
                                      0x05670080, PROBE_X, PROBE_Y,
                                      PROBE_SIZE, PROBE_SIZE, 0, 0xd, 0 },
                        11);
    kona_command_kicked(s, 0x1ee, (uint32_t[]){ PROBE_X, PROBE_Y, 0 }, 3);
    kona_command(s, 0xe3, (uint32_t[]){ 0, 0, PROBE_SIZE, PROBE_SIZE }, 4);
    kona_command(s, 0x25, (uint32_t[]){ 0xbd0 }, 1);
    kona_command(s, 0xc0, (uint32_t[]){ 0x1d00 }, 1);
    kona_command_f(s, 0x1d, (float[]){ r, g, b, 1 }, 4);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
}

/*
 * The pixels of a binary PPM, its header's comment lines skipped; its
 * size must be width by height with a maximum of 255.
 */
static uint8_t *ppm_pixels(const char *data, gsize len, unsigned width,
                           unsigned height)
{
    const char *p = data, *end = data + len;
    unsigned v[3];

    g_assert_true(len > 2 && !memcmp(p, "P6", 2));
    p += 2;
    for (int i = 0; i < 3; i++) {
        const char *next;
        unsigned long value;

        while (p < end && (g_ascii_isspace(*p) || *p == '#')) {
            if (*p == '#') {
                while (p < end && *p != '\n') {
                    p++;
                }
            } else {
                p++;
            }
        }
        g_assert_cmpint(qemu_strtoul(p, &next, 10, &value), ==, 0);
        g_assert_cmpuint(value, <=, UINT_MAX);
        v[i] = value;
        p = next;
    }
    g_assert_cmpuint(v[0], ==, width);
    g_assert_cmpuint(v[1], ==, height);
    g_assert_cmpuint(v[2], ==, 255);
    p++;
    g_assert_cmpuint(end - p, ==, (gsize)width * height * 3);
    return g_memdup2(p, end - p);
}

/* The console as a screendump's pixels, FB_WIDTH by FB_HEIGHT. */
static uint8_t *kona_screendump(QTestState *s)
{
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *data = NULL;
    GError *err = NULL;
    gsize len = 0;

    dir = g_dir_make_tmp("sn0-kona-3d-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(dir, "frame.ppm", NULL);
    qtest_qmp_assert_success(s,
        "{'execute': 'screendump', 'arguments': {'filename': %s, "
        "'device': 'kona0'}}", path);
    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    g_unlink(path);
    g_rmdir(dir);
    return ppm_pixels(data, len, FB_WIDTH, FB_HEIGHT);
}

/*
 * An expected window image: PROBE_SIZE square, three bytes a pixel, the
 * top row first as a screendump has it, so window row y is image row
 * PROBE_SIZE - 1 - y.
 */
typedef struct Expected {
    uint8_t rgb[PROBE_SIZE * PROBE_SIZE * 3];
} Expected;

static void expected_clear(Expected *e, uint32_t rgb)
{
    for (unsigned i = 0; i < PROBE_SIZE * PROBE_SIZE; i++) {
        e->rgb[3 * i] = rgb >> 16;
        e->rgb[3 * i + 1] = rgb >> 8;
        e->rgb[3 * i + 2] = rgb;
    }
}

/* Column-major 4x4 matrices and the OpenGL 1.2.1 ones of section 2.10. */
typedef struct Mat {
    double m[16];
} Mat;

static Mat mat_identity(void)
{
    Mat r = { { 0 } };

    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

static Mat mat_mul(Mat a, Mat b)
{
    Mat r;

    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            double v = 0;

            for (int k = 0; k < 4; k++) {
                v += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = v;
        }
    }
    return r;
}

static Mat mat_ortho(double l, double r, double b, double t, double n,
                     double f)
{
    Mat m = mat_identity();

    m.m[0] = 2 / (r - l);
    m.m[5] = 2 / (t - b);
    m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -(f + n) / (f - n);
    return m;
}

static Mat mat_frustum(double l, double r, double b, double t, double n,
                       double f)
{
    Mat m = { { 0 } };

    m.m[0] = 2 * n / (r - l);
    m.m[5] = 2 * n / (t - b);
    m.m[8] = (r + l) / (r - l);
    m.m[9] = (t + b) / (t - b);
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1;
    m.m[14] = -2 * f * n / (f - n);
    return m;
}

static Mat mat_translate(double x, double y, double z)
{
    Mat m = mat_identity();

    m.m[12] = x;
    m.m[13] = y;
    m.m[14] = z;
    return m;
}

static Mat mat_scale(double x, double y, double z)
{
    Mat m = mat_identity();

    m.m[0] = x;
    m.m[5] = y;
    m.m[10] = z;
    return m;
}

/* A rotation of degrees about the unit z axis. */
static Mat mat_rotate_z(double degrees)
{
    Mat m = mat_identity();
    double a = degrees * M_PI / 180;

    m.m[0] = cos(a);
    m.m[1] = sin(a);
    m.m[4] = -sin(a);
    m.m[5] = cos(a);
    return m;
}

/* An object vertex's window x and y under projection p and modelview mv. */
static void to_window(Mat p, Mat mv, const double v[3], double w[2])
{
    Mat pm = mat_mul(p, mv);
    double c[4];

    for (int row = 0; row < 4; row++) {
        c[row] = pm.m[row] * v[0] + pm.m[4 + row] * v[1] +
                 pm.m[8 + row] * v[2] + pm.m[12 + row];
    }
    g_assert_cmpfloat(c[3], >, 0);
    for (int k = 0; k < 2; k++) {
        double ndc = c[k] / c[3];

        g_assert_cmpfloat(fabs(ndc), <, 1);
        w[k] = (ndc + 1) * PROBE_SIZE / 2;
    }
}

/*
 * Fill a convex polygon of window vertices into the expected image by the
 * polygon rule (OpenGL 1.2.1, 3.5.1): a pixel whose centre is inside.
 * Asserts that every centre an edge could decide lies EDGE_MARGIN clear
 * of it.
 */
static void expected_polygon(Expected *e, const double (*w)[2], unsigned n,
                             uint32_t rgb)
{
    double area = 0;

    for (unsigned k = 0; k < n; k++) {
        const double *a = w[k], *b = w[(k + 1) % n];

        area += a[0] * b[1] - b[0] * a[1];
    }
    g_assert_cmpfloat(fabs(area), >, 1);
    for (int y = 0; y < PROBE_SIZE; y++) {
        for (int x = 0; x < PROBE_SIZE; x++) {
            double px = x + 0.5, py = y + 0.5, d[16];
            bool inside = true;
            uint8_t *out;

            g_assert_cmpuint(n, <=, 16);
            for (unsigned k = 0; k < n; k++) {
                const double *a = w[k], *b = w[(k + 1) % n];
                double len = hypot(b[0] - a[0], b[1] - a[1]);

                d[k] = ((b[0] - a[0]) * (py - a[1]) -
                        (b[1] - a[1]) * (px - a[0])) / len;
                if (area < 0) {
                    d[k] = -d[k];
                }
                inside &= d[k] > 0;
            }
            for (unsigned k = 0; k < n; k++) {
                bool others = true;

                for (unsigned j = 0; j < n; j++) {
                    others &= j == k || d[j] > -EDGE_MARGIN;
                }
                if (others && fabs(d[k]) < EDGE_MARGIN) {
                    g_error("pixel centre (%d.5, %d.5) is %g from edge %u",
                            x, y, d[k], k);
                }
            }
            if (!inside) {
                continue;
            }
            out = &e->rgb[((PROBE_SIZE - 1 - y) * PROBE_SIZE + x) * 3];
            out[0] = rgb >> 16;
            out[1] = rgb >> 8;
            out[2] = rgb;
        }
    }
}

/* Object vertices v under p and mv, filled in rgb. */
static void expected_object(Expected *e, Mat p, Mat mv, const double (*v)[3],
                            unsigned n, uint32_t rgb)
{
    double w[16][2];

    for (unsigned k = 0; k < n; k++) {
        to_window(p, mv, v[k], w[k]);
    }
    expected_polygon(e, w, n, rgb);
}

/*
 * The probe's window as the display shows it compared byte for byte with
 * the expected image. On a mismatch the first differing pixel is named.
 */
static void assert_window_matches(QTestState *s, const Expected *e)
{
    g_autofree uint8_t *frame = kona_screendump(s);
    const unsigned top = FB_HEIGHT - PROBE_Y - PROBE_SIZE;

    for (unsigned r = 0; r < PROBE_SIZE; r++) {
        const uint8_t *got = frame + ((top + r) * FB_WIDTH + PROBE_X) * 3;
        const uint8_t *want = e->rgb + r * PROBE_SIZE * 3;

        for (unsigned x = 0; x < PROBE_SIZE; x++) {
            if (memcmp(got + 3 * x, want + 3 * x, 3)) {
                g_error("window pixel (%u, %u) shows %02x%02x%02x, expected"
                        " %02x%02x%02x", x, PROBE_SIZE - 1 - r,
                        got[3 * x], got[3 * x + 1], got[3 * x + 2],
                        want[3 * x], want[3 * x + 1], want[3 * x + 2]);
            }
        }
    }
}

static void gl_begin(QTestState *s, uint32_t mode)
{
    kona_command(s, 0x57 + mode, (uint32_t[]){ 0 }, 1);
}

static void gl_end(QTestState *s)
{
    kona_command(s, 0x71, (uint32_t[]){ 0 }, 1);
}

static void gl_vertex2f(QTestState *s, double x, double y)
{
    kona_command_f(s, 0x8c, (float[]){ x, y }, 2);
}

/*
 * glOrtho's half-size view: the projection is glOrtho(-2, 2, -2, 2, -1, 1)
 * sent as six doubles, so a triangle in object coordinates covers half
 * the window it would under the identity; its colour is a packed
 * glColor3ub. Then a glRectf, 0x3 with its count 14, four vertices and
 * the end, in a colour from glColor4f. A model that ignored the matrix
 * draws the triangle twice the size; one that dropped the packed colour
 * draws it white.
 */
static void test_ortho_half_size(void)
{
    static const double tri[3][3] = {
        { -1.68, -1.3, 0 }, { 1.18, -0.9, 0 }, { 0.15, 1.45, 0 },
    };
    static const double rect[4][3] = {
        { 0.3, 0.35, 0 }, { 1.7, 0.35, 0 }, { 1.7, 1.65, 0 },
        { 0.3, 1.65, 0 },
    };
    Mat p = mat_ortho(-2, 2, -2, 2, -1, 1);
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    kona_command(qts, 0xd0, (uint32_t[]){ 0x1701 }, 1);
    kona_command(qts, 0xd2, (uint32_t[]){ 0 }, 1);
    kona_command_d(qts, 0xd7, (double[]){ -2, 2, -2, 2, -1, 1 }, 6);
    kona_command(qts, 0xd0, (uint32_t[]){ 0x1700 }, 1);
    kona_command(qts, 0xd2, (uint32_t[]){ 0 }, 1);
    qtest_writel(qts, REG_COMMAND, 0xffff0066);
    qtest_writel(qts, REG_COMMAND, 0x00000000);
    gl_begin(qts, 4);
    for (int k = 0; k < 3; k++) {
        gl_vertex2f(qts, tri[k][0], tri[k][1]);
    }
    gl_end(qts);
    kona_command_f(qts, 0x6b, (float[]){ 0, 1, 0, 1 }, 4);
    kona_command(qts, 0x3, (uint32_t[]){ 14 }, 1);
    for (int k = 0; k < 4; k++) {
        gl_vertex2f(qts, rect[k][0], rect[k][1]);
    }
    gl_end(qts);

    expected_clear(e, 0x0000ff);
    expected_object(e, p, mat_identity(), tri, 3, 0xffff00);
    expected_object(e, p, mat_identity(), rect, 4, 0x00ff00);
    assert_window_matches(qts, e);
}

/*
 * The modelview stack: glPushMatrix, glTranslatef and glRotatef(30, 0, 0,
 * 1), a red triangle, glPopMatrix, then a green triangle with the same
 * vertices moved, drawn under the identity again. A model that ignored
 * the rotation, or whose pop kept it, draws them elsewhere.
 */
static void test_rotated_triangle(void)
{
    static const double tri[3][3] = {
        { -0.55, -0.35, 0 }, { 0.45, -0.25, 0 }, { 0.1, 0.6, 0 },
    };
    static const double other[3][3] = {
        { -0.95, -0.95, 0 }, { -0.35, -0.85, 0 }, { -0.7, -0.4, 0 },
    };
    Mat mv = mat_mul(mat_translate(0.25, 0.2, 0), mat_rotate_z(30));
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 0);
    kona_command(qts, 0xd9, (uint32_t[]){ 0 }, 1);
    kona_command_f(qts, 0xdf, (float[]){ 0.25, 0.2, 0 }, 3);
    kona_command_f(qts, 0xdb, (float[]){ 30, 0, 0, 1 }, 4);
    kona_command_f(qts, 0x63, (float[]){ 1, 0, 0 }, 3);
    gl_begin(qts, 4);
    for (int k = 0; k < 3; k++) {
        kona_command_f(qts, 0x90, (float[]){ tri[k][0], tri[k][1], 0 }, 3);
    }
    gl_end(qts);
    kona_command(qts, 0xd8, (uint32_t[]){ 0 }, 1);
    kona_command_f(qts, 0x63, (float[]){ 0, 1, 0 }, 3);
    gl_begin(qts, 4);
    for (int k = 0; k < 3; k++) {
        gl_vertex2f(qts, other[k][0], other[k][1]);
    }
    gl_end(qts);

    expected_clear(e, 0);
    expected_object(e, mat_identity(), mv, tri, 3, 0xff0000);
    expected_object(e, mat_identity(), mat_identity(), other, 3, 0x00ff00);
    assert_window_matches(qts, e);
}

/*
 * A perspective-projected quad: glFrustum(-1, 1, -1, 1, 1, 10) as doubles,
 * then glTranslated(0, 0, -3), glRotated(20, 0, 0, 1) and glMultMatrixd of
 * a scale by 1.25 and 0.75, and a GL_QUADS of four glVertex3f, each from
 * the corner of a square in the plane z = 0. A model without the
 * projection's divide by w draws the quad three times the size.
 */
static void test_perspective_quad(void)
{
    static const double quad[4][3] = {
        { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 },
    };
    double scale[16] = { 0 };
    Mat p = mat_frustum(-1, 1, -1, 1, 1, 10);
    Mat mv = mat_mul(mat_mul(mat_translate(0, 0, -3), mat_rotate_z(20)),
                     mat_scale(1.25, 0.75, 1));
    g_autofree Expected *e = g_new(Expected, 1);

    scale[0] = 1.25;
    scale[5] = 0.75;
    scale[10] = 1;
    scale[15] = 1;
    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 0);
    kona_command(qts, 0xd0, (uint32_t[]){ 0x1701 }, 1);
    kona_command_d(qts, 0xd1, (double[]){ -1, 1, -1, 1, 1, 10 }, 6);
    kona_command(qts, 0xd0, (uint32_t[]){ 0x1700 }, 1);
    kona_command_d(qts, 0xde, (double[]){ 0, 0, -3 }, 3);
    kona_command_d(qts, 0xda, (double[]){ 20, 0, 0, 1 }, 4);
    kona_command_d(qts, 0xd6, scale, 16);
    kona_command_f(qts, 0x63, (float[]){ 1, 1, 1 }, 3);
    gl_begin(qts, 7);
    for (int k = 0; k < 4; k++) {
        kona_command_f(qts, 0x90,
                       (float[]){ quad[k][0], quad[k][1], quad[k][2] }, 3);
    }
    gl_end(qts);

    expected_clear(e, 0);
    expected_object(e, p, mv, quad, 4, 0xffffff);
    assert_window_matches(qts, e);
}

/*
 * One lit flat triangle. Light 0 is directional, its position (0, 1, 0, 0)
 * sent while the modelview is glRotatef(90, 1, 0, 0), which carries it to
 * the eye direction (0, 0, 1); the modelview is the identity again before
 * the triangle. The material's diffuse colour is (0.8, 0.4, 0.2) and the
 * normal (0, 0.6, 0.8). The expected colour is computed here from OpenGL
 * 1.2.1's lighting equation (2.13.1) with every other parameter at its
 * initial value: the light model's ambient 0.2 times the material's
 * ambient 0.2, plus n.L times the diffuse colour times the light's
 * diffuse 1; no specular term, the material's specular being 0. A model
 * that did not light gives the current colour; one that did not carry
 * the light position through the modelview gets n.L = 0.6 instead of 0.8.
 */
static void test_lit_flat_triangle(void)
{
    static const double tri[3][3] = {
        { -0.8, -0.7, 0 }, { 0.7, -0.6, 0 }, { -0.1, 0.8, 0 },
    };
    const double diffuse[3] = { 0.8, 0.4, 0.2 };
    const double n_dot_l = 0.6 * 0 + 0.8 * 1;
    g_autofree Expected *e = g_new(Expected, 1);
    uint32_t rgb = 0;

    for (int k = 0; k < 3; k++) {
        double c = 0.2 * 0.2 + n_dot_l * diffuse[k] * 1.0;
        double v = c * 255 + 0.5;

        /* Clear of a rounding tie, so any sound arithmetic agrees. */
        g_assert_cmpfloat(fabs(v - floor(v) - 0.5), <, 0.45);
        rgb = rgb << 8 | (uint32_t)floor(v);
    }

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 0);
    kona_command(qts, 0xd9, (uint32_t[]){ 0 }, 1);
    kona_command_f(qts, 0xdb, (float[]){ 90, 1, 0, 0 }, 4);
    kona_command(qts, 0x7, (uint32_t[]){ 0x4000, 0x1203, F0, F1, F0, F0 }, 6);
    kona_command(qts, 0xd8, (uint32_t[]){ 0 }, 1);
    kona_command(qts, 0x97, (uint32_t[]){ 0x408, 0x1201, fbits(0.8),
                                         fbits(0.4), fbits(0.2), F1 }, 6);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb50 }, 1);
    kona_command(qts, 0x24, (uint32_t[]){ 0x4000 }, 1);
    kona_command_f(qts, 0x63, (float[]){ 1, 1, 1 }, 3);
    kona_command_f(qts, 0x78, (float[]){ 0, 0.6, 0.8 }, 3);
    gl_begin(qts, 4);
    for (int k = 0; k < 3; k++) {
        gl_vertex2f(qts, tri[k][0], tri[k][1]);
    }
    gl_end(qts);

    g_assert_cmphex(rgb, ==, 0xad5c33);
    expected_clear(e, 0);
    expected_object(e, mat_identity(), mat_identity(), tri, 3, rgb);
    assert_window_matches(qts, e);
}

/*
 * A packed glColor3ub caught by a migration between its two words: the
 * selector word, carrying red 0xff and green 0, arrives on the source and
 * blue 0xff on the destination, which then draws a triangle magenta. A
 * destination that lost the selector word's top half draws it blue.
 */
static void test_packed_colour_across_migration(void)
{
    static const double tri[3][3] = {
        { -0.8, -0.7, 0 }, { 0.7, -0.6, 0 }, { -0.1, 0.8, 0 },
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-3d-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    gl_make_current(from);
    gl_window_cleared(from, 0, 0, 0);
    qtest_writel(from, REG_COMMAND, 0xff000066);
    sgi_migrate_stopped(from, uri);

    qtest_writel(to, REG_COMMAND, 0xff000000);
    gl_begin(to, 4);
    for (int k = 0; k < 3; k++) {
        gl_vertex2f(to, tri[k][0], tri[k][1]);
    }
    gl_end(to);
    expected_clear(e, 0);
    expected_object(e, mat_identity(), mat_identity(), tri, 3, 0xff00ff);
    assert_window_matches(to, e);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/* A flat triangle of glVertex3f at depth z in the current colour. */
static void gl_triangle_z(QTestState *s, const double (*v)[3], float z)
{
    gl_begin(s, 4);
    for (int k = 0; k < 3; k++) {
        kona_command_f(s, 0x90, (float[]){ v[k][0], v[k][1], z }, 3);
    }
    gl_end(s);
}

/*
 * Two overlapping triangles with a 24-bit depth buffer, the nearer one
 * drawn first. Under GL_LESS, after a clear of colour and depth to 1.0,
 * the farther one is hidden where they overlap. Then, after glClearDepth
 * of 0 sent as a double and a clear of both buffers, under GL_GREATER
 * the triangle drawn second, now the nearer, is hidden instead. A model
 * without the depth test shows the second triangle on top both times;
 * one that ignored glClearDepth draws nothing under GL_GREATER.
 */
static void test_depth_less_and_greater(void)
{
    static const double a[3][3] = {
        { -0.7, -0.6, 0 }, { 0.5, -0.5, 0 }, { -0.1, 0.6, 0 },
    };
    static const double b[3][3] = {
        { -0.3, -0.2, 0 }, { 0.8, -0.1, 0 }, { 0.3, 0.75, 0 },
    };
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    gl_make_current_depth(qts, 24);
    gl_window_cleared(qts, 0, 0, 0);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4100 }, 1);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);
    kona_command(qts, 0xce, (uint32_t[]){ 0x201 }, 1);
    kona_command_f(qts, 0x63, (float[]){ 1, 0, 0 }, 3);
    gl_triangle_z(qts, a, -0.5);
    kona_command_f(qts, 0x63, (float[]){ 0, 1, 0 }, 3);
    gl_triangle_z(qts, b, 0.5);
    expected_clear(e, 0);
    expected_object(e, mat_identity(), mat_identity(), b, 3, 0x00ff00);
    expected_object(e, mat_identity(), mat_identity(), a, 3, 0xff0000);
    assert_window_matches(qts, e);

    kona_command_d(qts, 0x1f, (double[]){ 0 }, 1);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4100 }, 1);
    kona_command(qts, 0xce, (uint32_t[]){ 0x204 }, 1);
    kona_command_f(qts, 0x63, (float[]){ 1, 0, 0 }, 3);
    gl_triangle_z(qts, a, 0.5);
    kona_command_f(qts, 0x63, (float[]){ 0, 1, 0 }, 3);
    gl_triangle_z(qts, b, -0.5);
    assert_window_matches(qts, e);
}

/*
 * The same triangles and depth test in a context whose visual has no
 * depth buffer: GL gives it no depth test, so the second triangle is on
 * top where they overlap.
 */
static void test_depth_without_buffer(void)
{
    static const double a[3][3] = {
        { -0.7, -0.6, 0 }, { 0.5, -0.5, 0 }, { -0.1, 0.6, 0 },
    };
    static const double b[3][3] = {
        { -0.3, -0.2, 0 }, { 0.8, -0.1, 0 }, { 0.3, 0.75, 0 },
    };
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 0);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4100 }, 1);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);
    kona_command(qts, 0xce, (uint32_t[]){ 0x201 }, 1);
    kona_command_f(qts, 0x63, (float[]){ 1, 0, 0 }, 3);
    gl_triangle_z(qts, a, -0.5);
    kona_command_f(qts, 0x63, (float[]){ 0, 1, 0 }, 3);
    gl_triangle_z(qts, b, 0.5);
    expected_clear(e, 0);
    expected_object(e, mat_identity(), mat_identity(), a, 3, 0xff0000);
    expected_object(e, mat_identity(), mat_identity(), b, 3, 0x00ff00);
    assert_window_matches(qts, e);
}

/*
 * A red triangle with alpha 0.25 blended over a blue clear by
 * glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA): red 0.25 and blue
 * 0.75 of 255, 63.75 and 191.25, which round to 64 and 191 whether the
 * source is quantized first or not. A model without blending draws it
 * plain red.
 */
static void test_blended_triangle(void)
{
    static const double tri[3][3] = {
        { -0.62, -0.65, 0 }, { 0.7, -0.41, 0 }, { 0.1, 0.7, 0 },
    };
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    kona_command(qts, 0x24, (uint32_t[]){ 0xbe2 }, 1);
    kona_command(qts, 0xcb, (uint32_t[]){ 0x302, 0x303 }, 2);
    kona_command_f(qts, 0x6b, (float[]){ 1, 0, 0, 0.25 }, 4);
    gl_begin(qts, 4);
    for (int k = 0; k < 3; k++) {
        gl_vertex2f(qts, tri[k][0], tri[k][1]);
    }
    gl_end(qts);
    expected_clear(e, 0x0000ff);
    expected_object(e, mat_identity(), mat_identity(), tri, 3, 0x4000bf);
    assert_window_matches(qts, e);
}

/*
 * Back-face culling: with GL_CULL_FACE enabled and glCullFace(GL_BACK),
 * a clockwise triangle draws nothing and a counter-clockwise one draws.
 * After glFrontFace(GL_CW) the clockwise triangle, drawn again in yellow,
 * is the front one and draws.
 */
static void test_culled_triangle(void)
{
    static const double cw[3][3] = {
        { -0.9, 0.1, 0 }, { -0.3, 0.85, 0 }, { -0.15, 0.2, 0 },
    };
    static const double ccw[3][3] = {
        { 0.1, -0.9, 0 }, { 0.85, -0.75, 0 }, { 0.4, -0.15, 0 },
    };
    g_autofree Expected *e = g_new(Expected, 1);
    g_autofree Expected *before = g_new(Expected, 1);

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 0);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb44 }, 1);
    kona_command(qts, 0xb6, (uint32_t[]){ 0x405 }, 1);
    kona_command_f(qts, 0x63, (float[]){ 1, 0, 0 }, 3);
    gl_triangle_z(qts, cw, 0);
    kona_command_f(qts, 0x63, (float[]){ 0, 1, 0 }, 3);
    gl_triangle_z(qts, ccw, 0);
    expected_clear(before, 0);
    expected_object(before, mat_identity(), mat_identity(), ccw, 3,
                    0x00ff00);
    assert_window_matches(qts, before);

    kona_command(qts, 0xba, (uint32_t[]){ 0x900 }, 1);
    kona_command_f(qts, 0x63, (float[]){ 1, 1, 0 }, 3);
    gl_triangle_z(qts, cw, 0);
    *e = *before;
    expected_object(e, mat_identity(), mat_identity(), cw, 3, 0xffff00);
    assert_window_matches(qts, e);
}

static uint32_t guest_word(QTestState *s, uint64_t addr)
{
    uint8_t bytes[4];

    qtest_memread(s, addr, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

/* The client's wait: word 0 goes non-zero, and the client clears it. */
static void assert_reply_posted(QTestState *s)
{
    g_assert_cmphex(guest_word(s, RETURN_PAGE), !=, 0);
    qtest_memset(s, RETURN_PAGE, 0, 4);
}

/*
 * A blue window with a red rectangle from window (11, 21) to (60, 70),
 * drawn by glRectf under glOrtho(0, 128, 0, 128, -1, 1), so object and
 * window coordinates agree.
 */
static void gl_rect_scene(QTestState *s)
{
    static const float rect[4][2] = {
        { 11, 21 }, { 60, 21 }, { 60, 70 }, { 11, 70 },
    };

    gl_window_cleared(s, 0, 0, 1);
    kona_command(s, 0xd0, (uint32_t[]){ 0x1701 }, 1);
    kona_command_d(s, 0xd7, (double[]){ 0, 128, 0, 128, -1, 1 }, 6);
    kona_command(s, 0xd0, (uint32_t[]){ 0x1700 }, 1);
    kona_command_f(s, 0x63, (float[]){ 1, 0, 0 }, 3);
    kona_command(s, 0x3, (uint32_t[]){ 14 }, 1);
    for (int k = 0; k < 4; k++) {
        kona_command_f(s, 0x8c, rect[k], 2);
    }
    gl_end(s);
}

/* Native RGB bytes are dense at byte 8, independently of pack stride. */
static void test_read_pixels(void)
{
    static const uint8_t rgb[] = {
        0, 0, 0xff, 0, 0, 0xff, 0, 0, 0xff,
        0, 0, 0xff, 0xff, 0, 0, 0xff, 0, 0,
    };
    uint8_t got[4 + sizeof(rgb) + 4];

    gl_reset();
    gl_make_current(qts);
    gl_rect_scene(qts);
    for (unsigned low = 0; low < 4; low++) {
        qtest_memset(qts, RETURN_PAGE, 0xa5, 0x4000);
        qtest_memset(qts, RETURN_PAGE, 0, 4);
        kona_command(qts, 0x39,
                     (uint32_t[]){ 10, 20, 3, 2, 4, 1, low }, 7);
        assert_reply_posted(qts);
        qtest_memread(qts, RETURN_PAGE + 4, got, sizeof(got));
        for (unsigned k = 0; k < 4; k++) {
            g_assert_cmphex(got[k], ==, 0xa5);
            g_assert_cmphex(got[4 + sizeof(rgb) + k], ==, 0xa5);
        }
        g_assert_cmpmem(got + 4, sizeof(rgb), rgb, sizeof(rgb));
    }
    for (unsigned low = 0; low < 4; low++) {
        uint8_t dense[360], expected[360];

        for (unsigned r = 0; r < 8; r++) {
            for (unsigned c = 0; c < 15; c++) {
                unsigned at = (r * 15 + c) * 3;
                bool red = r >= 1 && c >= 1;

                expected[at] = red ? 0xff : 0;
                expected[at + 1] = 0;
                expected[at + 2] = red ? 0 : 0xff;
            }
        }
        qtest_memset(qts, RETURN_PAGE + 8, 0xa5, 364);
        kona_command(qts, 0x39,
                     (uint32_t[]){ 10, 20, 15, 8, 4, 1, low }, 7);
        assert_reply_posted(qts);
        qtest_memread(qts, RETURN_PAGE + 8, dense, sizeof(dense));
        g_assert_cmpmem(dense, sizeof(dense), expected, sizeof(expected));
        g_assert_cmphex(guest_word(qts, RETURN_PAGE + 368), ==, 0xa5a5a5a5);
    }
}

/* The conservative small-return cap is 16368 bytes, with no truncation. */
static void test_read_pixels_capacity(void)
{
    uint8_t payload[16368];

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    kona_command_kicked(qts, 0x1f3,
                        (uint32_t[]){ PROBE_X, PROBE_Y, 0x05670080,
                                      0x05670080, PROBE_X, PROBE_Y,
                                      682, 8, 0, 0xd, 0 }, 11);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    qtest_memset(qts, RETURN_PAGE, 0xa5, 0x4000);
    qtest_memset(qts, RETURN_PAGE, 0, 4);
    kona_command(qts, 0x39, (uint32_t[]){ 0, 0, 682, 8, 4, 1, 0 }, 7);
    assert_reply_posted(qts);
    qtest_memread(qts, RETURN_PAGE + 8, payload, sizeof(payload));
    for (unsigned k = 0; k < sizeof(payload); k += 3) {
        g_assert_cmphex(payload[k], ==, 0);
        g_assert_cmphex(payload[k + 1], ==, 0);
        g_assert_cmphex(payload[k + 2], ==, 0xff);
    }
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 16376), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 16380), ==, 0xa5a5a5a5);
}

static void assert_read_colour(QTestState *s, uint32_t rgb)
{
    uint8_t got[18];

    qtest_memset(s, RETURN_PAGE, 0, 4);
    kona_command(s, 0x39, (uint32_t[]){ 0, 0, 3, 2, 4, 1, 0 }, 7);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 8, got, sizeof(got));
    for (unsigned k = 0; k < sizeof(got); k += 3) {
        g_assert_cmphex(got[k], ==, rgb >> 16);
        g_assert_cmphex(got[k + 1], ==, (rgb >> 8) & 0xff);
        g_assert_cmphex(got[k + 2], ==, rgb & 0xff);
    }
}

static void assert_read_buffer(QTestState *s, uint32_t buffer)
{
    kona_command(s, 0x4c, (uint32_t[]){ 0xc02 }, 1);
    assert_reply_posted(s);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, buffer);
}

static void test_read_selection(void)
{
    static const uint32_t invalid[] = { 0, 0x402, 0x405, 0x401, 0x403,
                                        0x406, 0x407, 0x408, 0x409, 0xdead };

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    assert_read_buffer(qts, 0x404);
    assert_read_colour(qts, 0x0000ff);
    for (unsigned k = 0; k < ARRAY_SIZE(invalid); k++) {
        kona_command(qts, 0x37, &invalid[k], 1);
        qtest_memset(qts, RETURN_PAGE, 0, 4);
        kona_command(qts, 0x39, (uint32_t[]){ 0, 0, 3, 2, 4, 1, 0 }, 7);
        g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 0);
        kona_command(qts, 0x4c, (uint32_t[]){ 0xc02 }, 1);
        g_assert_cmphex(guest_word(qts, RETURN_PAGE), ==, 0);
        kona_command(qts, 0x37, (uint32_t[]){ 0x404 }, 1);
        assert_read_buffer(qts, 0x404);
        assert_read_colour(qts, 0x0000ff);
    }
    kona_command(qts, 0x37, (uint32_t[]){ 0x400 }, 1);
    assert_read_buffer(qts, 0x400);
    assert_read_colour(qts, 0x0000ff);
}

/*
 * glGet's 0x4c: the modelview matrix glLoadMatrixf set, as sixteen float
 * words from byte 8; the viewport as four integers; the depth range
 * glDepthRange set as native binary64; and GL_DEPTH_TEST's enable as 1.
 */
static void test_get_queries(void)
{
    uint32_t m[16];

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 0);
    for (int k = 0; k < 16; k++) {
        m[k] = fbits(k + 1.5f);
    }
    kona_command(qts, 0xd3, m, 16);
    kona_command(qts, 0xcf,
                 (uint32_t[]){ 0x3fd00000, 1, 0x3fe00000, 1 }, 4);
    kona_command(qts, 0xe3, (uint32_t[]){ 7, 11, 113, 97 }, 4);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);

    qtest_memset(qts, RETURN_PAGE + 4, 0xa5, 0x3ffc);
    kona_command(qts, 0x4c, (uint32_t[]){ 0xba6 }, 1);
    assert_reply_posted(qts);
    for (int k = 0; k < 16; k++) {
        g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8 + 4 * k), ==, m[k]);
    }
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 72), ==, 0xa5a5a5a5);
    qtest_memset(qts, RETURN_PAGE + 8, 0xa5, 0x3ff8);
    kona_command(qts, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 7);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 12), ==, 11);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 16), ==, 113);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 20), ==, 97);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 24), ==, 0xa5a5a5a5);
    qtest_memset(qts, RETURN_PAGE + 8, 0xa5, 0x3ff8);
    kona_command(qts, 0x4c, (uint32_t[]){ 0xb70 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 0x3fd00000);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 12), ==, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 16), ==, 0x3fe00000);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 20), ==, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 24), ==, 0xa5a5a5a5);
    qtest_memset(qts, RETURN_PAGE + 8, 0xa5, 0x3ff8);
    kona_command(qts, 0x4c, (uint32_t[]){ 0xb71 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 12), ==, 0xa5a5a5a5);
}

/* Sentinels catch partial payload writes as well as a false ready word. */
static void prepare_refused_reply(QTestState *s)
{
    qtest_memset(s, RETURN_PAGE, 0xa5, 0x4000);
    qtest_memset(s, RETURN_PAGE, 0, 4);
}

static void assert_reply_unchanged(QTestState *s)
{
    uint8_t expected[0x4000], got[0x4000];

    memset(expected, 0xa5, sizeof(expected));
    memset(expected, 0, 4);
    qtest_memread(s, RETURN_PAGE, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
}

static void assert_source_refused(QTestState *s)
{
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]){ 0, 0, 3, 2, 4, 1, 0 }, 7);
    assert_reply_unchanged(s);
    kona_command(s, 0x4c, (uint32_t[]){ 0xc02 }, 1);
    assert_reply_unchanged(s);
}

static void test_read_visual_records(void)
{
    static const uint32_t config[10] = { 0, 12 };
    static const uint32_t invalid[][2] = {
        { 0, 0x100 }, { 1, 0 }, { 1, 7 }, { 1, 16 }, { 2, 8 },
        { 3, 2 }, { 4, 1 }, { 5, 1 }, { 6, 33 }, { 7, 1 },
        { 8, 16 }, { 9, 1 },
    };
    uint32_t record[10];

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    for (unsigned k = 0; k < ARRAY_SIZE(invalid); k++) {
        memcpy(record, config, sizeof(record));
        record[invalid[k][0]] = invalid[k][1];
        kona_command(qts, 1, record, 10);
        assert_source_refused(qts);
        kona_command(qts, 1, config, 10);
        assert_read_colour(qts, 0x0000ff);
        kona_command(qts, 4, record, 10);
        assert_source_refused(qts);
        kona_command(qts, 4, config, 10);
        assert_read_colour(qts, 0x0000ff);
    }
    memcpy(record, config, sizeof(record));
    record[1] = 8;
    kona_command(qts, 1, record, 10);
    assert_source_refused(qts); /* Both valid, but complete records differ. */
    kona_command(qts, 4, record, 10);
    assert_read_colour(qts, 0x0000ff);
    record[6] = 32;
    kona_command(qts, 4, record, 10);
    assert_source_refused(qts);
    kona_command(qts, 1, record, 10);
    assert_read_colour(qts, 0x0000ff);
    kona_command(qts, 0x114, (uint32_t[]){ 0x12345678 }, 1);
    assert_source_refused(qts);
    kona_command(qts, 4, record, 10);
    kona_command(qts, 1, record, 10);
    kona_command(qts, 0x37, (uint32_t[]){ 0x404 }, 1);
    gl_window_cleared(qts, 0, 0, 1);
    /* Reconfiguration never clears unknown binding. */
    assert_source_refused(qts);
    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    assert_read_colour(qts, 0x0000ff);
}

/* The existing PROM/GL discriminator does not frame level 1 as GL data. */
static void test_read_ambiguous_level_preserves_source(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    kona_command(s, 1, (uint32_t[]){ 1, 12, 0, 0, 0, 0, 0, 0, 0, 0 }, 10);
    assert_read_buffer(s, 0x404);
    assert_read_colour(s, 0x0000ff);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found,
                    "sgi_kona_gl_buffer_config token 0x1 "), ==, 1);
    g_assert_nonnull(strstr(found, "textport expected a sync word"));
}

/* A slot command aborts partial GL framing, preserving complete records. */
static void test_read_partial_records(void)
{
    static const uint32_t config[10] = { 0, 12 };
    static const uint32_t different[10] = { 0, 8, 0, 1 };
    static const uint32_t selectors[] = { 1, 4, 0x114 };

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    for (unsigned k = 0; k < ARRAY_SIZE(selectors); k++) {
        kona_command(qts, selectors[k], different, k == 2 ? 0 : 5);
        qtest_writel(qts, KONA_REG(0xc1000c + 0x20 * 0x1dd), 0);
        assert_read_buffer(qts, 0x404);
        assert_read_colour(qts, 0x0000ff);
    }
    kona_command(qts, 4, config, 10);
    assert_read_colour(qts, 0x0000ff);
}

static void test_read_reply_order(void)
{
    g_autofree char *log = NULL, *found = NULL;
    const char *payload, *ready;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]){ 0, 0, 3, 2, 4, 1, 3 }, 7);
    assert_reply_posted(s);
    qtest_quit(s);
    found = kona_log_read(log);
    payload = strstr(found, "addr 0x320008 len 0x12\n");
    ready = strstr(found, "addr 0x320000 len 0x4\n");
    g_assert_nonnull(payload);
    g_assert_nonnull(ready);
    g_assert_true(payload < ready);
}

static void place_read_window(QTestState *s, int32_t x, int32_t y,
                               uint32_t width, uint32_t height)
{
    kona_command_kicked(s, 0x1f3,
                        (uint32_t[]){ x, y, 0x05670080, 0x05670080,
                                      x, y, width, height, 0, 0xd, 0 }, 11);
    kona_command_kicked(s, 0x1ee, (uint32_t[]){ x, y, 0 }, 3);
}

static void test_read_coordinates(void)
{
    static const int32_t origins[][2] = {
        { 273, 219 }, { 91, 307 }, { 0, 0 }, { 1264, 0 },
        { 0, 1008 }, { 1264, 1008 },
    };

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    kona_command(qts, 0x37, (uint32_t[]){ 0x404 }, 1);
    for (unsigned k = 0; k < ARRAY_SIZE(origins); k++) {
        place_read_window(qts, origins[k][0], origins[k][1], 16, 16);
        kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
        assert_read_colour(qts, 0x0000ff);
        /* Read coordinates do not pass through viewport or scissor. */
        kona_command(qts, 0xe3, (uint32_t[]){ 7, 9, 3, 2 }, 4);
        kona_command(qts, 0xc, (uint32_t[]){ 15, 15, 1, 1 }, 4);
        kona_command(qts, 0x24, (uint32_t[]){ 0xc11 }, 1);
        assert_read_colour(qts, 0x0000ff);
        kona_command(qts, 0x25, (uint32_t[]){ 0xc11 }, 1);
    }
    place_read_window(qts, -2, -1, 16, 16);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    kona_command(qts, 0x39, (uint32_t[]){ 2, 1, 3, 2, 4, 1, 0 }, 7);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 0x0000ff00);
    prepare_refused_reply(qts);
    kona_command(qts, 0x39, (uint32_t[]){ 1, 0, 3, 2, 4, 1, 0 }, 7);
    assert_reply_unchanged(qts);
    prepare_refused_reply(qts);
    kona_command(qts, 0x39,
                 (uint32_t[]){ INT32_MAX, INT32_MAX, 3, 2, 4, 1, 0 }, 7);
    assert_reply_unchanged(qts);
    place_read_window(qts, 1278, 1022, 16, 16);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    prepare_refused_reply(qts);
    kona_command(qts, 0x39, (uint32_t[]){ 0, 0, 3, 2, 4, 1, 0 }, 7);
    assert_reply_unchanged(qts);
    kona_command(qts, 0x39, (uint32_t[]){ 0, 0, 2, 2, 4, 1, 0 }, 7);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 0x0000ff00);
}

static void x_read_fixture_command(QTestState *s, uint32_t token,
                                   const uint32_t *args, unsigned count)
{
    qtest_writel(s, KONA_REG(0xc1000c + 0x20 * token), args[0]);
    for (unsigned k = 1; k < count; k++) {
        qtest_writel(s, KONA_REG(0xc1000c), args[k]);
    }
}

static void test_read_indexed_source_refused(void)
{
    QDict *reply;

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    x_read_fixture_command(qts, 0x12e, (uint32_t[]){ 0x2d07, 1, 0x304c1304 },
                            3);
    x_read_fixture_command(qts, 0x136, (uint32_t[]){ 0, 0, 1, 8, 1, 1, 0 }, 7);
    x_read_fixture_command(qts, 0x139,
                            (uint32_t[]){ PROBE_X + 2, 1023 - PROBE_Y - 1 }, 2);
    qtest_writel(qts, KONA_REG(0xc1000c), 0x07000000);
    x_read_fixture_command(qts, 0x13a, (uint32_t[]){ 0 }, 1);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane','value':2}}");
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-x','value':%u}}", PROBE_X + 2);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-y','value':%u}}", PROBE_Y + 1);
    reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane-word'}}");
    g_assert_cmpuint(qdict_get_int(reply, "return"), ==, 2);
    qobject_unref(reply);
    prepare_refused_reply(qts);
    kona_command(qts, 0x39, (uint32_t[]){ 0, 0, 3, 2, 4, 1, 0 }, 7);
    assert_reply_unchanged(qts); /* Last source pixel is indexed. */
    assert_read_buffer(qts, 0x404);
}

static void test_read_double_buffers(void)
{
    static const uint32_t double_config[10] = { 0, 12, 0, 1 };
    static const uint32_t single_config[10] = { 0, 12 };

    gl_reset();
    gl_make_current(qts);
    kona_command(qts, 1, double_config, 10);
    gl_window_cleared(qts, 0, 0, 1); /* Default back, blue. */
    kona_command(qts, 0xc4, (uint32_t[]){ 0x404 }, 1);
    kona_command_f(qts, 0x1d, (float[]){ 1, 0, 0, 1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    assert_read_buffer(qts, 0x405);
    assert_read_colour(qts, 0x0000ff);
    kona_command(qts, 0x37, (uint32_t[]){ 0x404 }, 1);
    assert_read_buffer(qts, 0x404);
    assert_read_colour(qts, 0xff0000);
    for (unsigned k = 0; k < 2; k++) {
        kona_command(qts, 0xc6, (uint32_t[]){ 0 }, 1);
        kona_command_kicked(qts, 0x1f4, (uint32_t[]){ 1, 1 }, 2);
        kona_command_kicked(qts, 0x172, (uint32_t[]){ 0 }, 1);
        kona_command_kicked(qts, 0x1e6, (uint32_t[]){ 0x800003, 0xd, 5 }, 3);
        kona_command_kicked(qts, 0x1e7, (uint32_t[]){ k + 1 }, 1);
        qtest_clock_step(qts, 20000000);
        assert_read_buffer(qts, 0x404);
        assert_read_colour(qts, k ? 0xff0000 : 0x0000ff);
        kona_command(qts, 0x37, (uint32_t[]){ 0x402 }, 1);
        assert_read_buffer(qts, 0x402);
        assert_read_colour(qts, k ? 0x0000ff : 0xff0000);
        kona_command(qts, 0x37, (uint32_t[]){ 0x404 }, 1);
    }
    kona_command(qts, 0x37, (uint32_t[]){ 0x405 }, 1);
    kona_command(qts, 1, single_config, 10);
    assert_source_refused(qts);
    kona_command(qts, 1, double_config, 10);
    assert_read_buffer(qts, 0x405);
    assert_read_colour(qts, 0x0000ff);
}

static void test_unknown_query_refused(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    prepare_refused_reply(s);
    kona_command(s, 0x4c, (uint32_t[]){ 0xd33 }, 1);
    assert_reply_unchanged(s);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "sgi-kona: glGet of 0xd33"));
}

/* Known wire metadata cannot stand in for an unknown state meaning. */
static void test_unmodeled_queries_refused(void)
{
    static const uint32_t names[] = { 0xb14, 0x81c4, 0x81c7, 0x2a00 };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    for (unsigned k = 0; k < ARRAY_SIZE(names); k++) {
        prepare_refused_reply(s);
        kona_command(s, 0x4c, &names[k], 1);
        assert_reply_unchanged(s);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_null(strstr(found, "sgi_kona_return_post token 0x4c "));
    g_assert_false(log_shows_misframing(found));
}

static void test_read_pixels_refused(gconstpointer opaque)
{
    const uint32_t *args = opaque;
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    gl_rect_scene(s);
    prepare_refused_reply(s);
    kona_command(s, 0x39, args, 7);
    assert_reply_unchanged(s);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    assert_reply_posted(s);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "sgi-kona: glReadPixels"));
}

/* Missing pages and failed bus writes must never publish a ready word. */
static void test_read_destination_refused(gconstpointer opaque)
{
    bool failed_access = GPOINTER_TO_INT(opaque);
    uint32_t config[10] = { 0, 0xc };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ physical_page_word(SAVE_AREA),
                                      tagged_offset_word(SAVE_AREA),
                                      0x10000 }, 3);
    kona_command(s, 1, config, 10);
    kona_command(s, 2, (uint32_t[]){ 0 }, 1);
    gl_rect_scene(s);
    prepare_refused_reply(s);
    if (failed_access) {
        /* Outside this fixture's 64 MiB RAM. */
        kona_command_kicked(s, 0x1f2,
                            (uint32_t[]){ physical_page_word(0x08000000),
                                          tagged_offset_word(0x08000000),
                                          0x4000 }, 3);
    }
    kona_command(s, 0x39, (uint32_t[]){ 10, 20, 3, 2, 4, 1, 0 }, 7);
    assert_reply_unchanged(s);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    assert_reply_posted(s);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_null(strstr(found, "sgi_kona_return_post token 0x39 "));
    g_assert_false(log_shows_misframing(found));
}

/* Ordinary queries start with ready clear and retain opaque word one. */
static void assert_attrib_depth(QTestState *s, uint8_t count)
{
    uint8_t bytes[12];
    const uint8_t expected[] = {
        0xa5, 0xa5, 0xa5, 0xa5, 0, 0, 0, count,
        0xa5, 0xa5, 0xa5, 0xa5,
    };

    qtest_memset(s, RETURN_PAGE, 0, 4);
    qtest_memset(s, RETURN_PAGE + 4, 0xa5, sizeof(bytes));
    kona_command(s, 0x4c, (uint32_t[]){ 0xbb0 }, 1);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 4, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
}

static void assert_hint(QTestState *s, unsigned hint, uint8_t mode)
{
    uint8_t bytes[12];
    const uint8_t expected[] = {
        0xa5, 0xa5, 0xa5, 0xa5, 0, 0, 0x11, mode,
        0xa5, 0xa5, 0xa5, 0xa5,
    };

    prepare_refused_reply(s);
    kona_command(s, 0x4c, (uint32_t[]){ 0xc50 + hint }, 1);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 4, bytes, sizeof(bytes));
    if (memcmp(bytes, expected, sizeof(bytes))) {
        g_test_message("hint %u: expected mode %u, payload 0x%08x", hint, mode,
                       ldl_be_p(bytes + 4));
    }
    g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
}

static void test_native_hints(void)
{
    gl_reset();
    gl_make_current(qts);
    for (unsigned k = 0; k < 5; k++) {
        assert_hint(qts, k, 0);
        for (unsigned mode = 0; mode < 3; mode++) {
            prepare_refused_reply(qts);
            kona_command(qts, 0xbb,
                         (uint32_t[]){ 0xc50 + k, 0x1100 + mode }, 2);
            assert_reply_unchanged(qts);
            assert_hint(qts, k, mode);
            assert_hint(qts, k, mode);
        }
        kona_command(qts, 0xbb, (uint32_t[]){ 0xc50 + k, 0x1100 }, 2);
    }
    qtest_writeq(qts, REG_COMMAND, (UINT64_C(0xbb) << 32) | 0xc50);
    qtest_writel(qts, REG_COMMAND, 0x1102);
    assert_hint(qts, 0, 2);
    qtest_writel(qts, REG_COMMAND, 0xbb);
    qtest_writeq(qts, REG_COMMAND, (UINT64_C(0xc50) << 32) | 0x1101);
    assert_hint(qts, 0, 1);
}

static void test_native_attrib_depth(void)
{
    gl_reset();
    gl_make_current(qts);
    assert_attrib_depth(qts, 0);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_attrib_depth(qts, 1);
    assert_attrib_depth(qts, 1);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_attrib_depth(qts, 2);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_attrib_depth(qts, 1);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_attrib_depth(qts, 0);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_attrib_depth(qts, 0);
}

static void test_native_attrib_depth_limits(void)
{
    gl_reset();
    gl_make_current(qts);
    for (unsigned k = 0; k < 16; k++) {
        kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    }
    assert_attrib_depth(qts, 16);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_attrib_depth(qts, 16);
    for (unsigned k = 0; k < 16; k++) {
        kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    }
    assert_attrib_depth(qts, 0);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_attrib_depth(qts, 0);
}

static void test_native_attrib_depth_reset(void)
{
    gl_reset();
    gl_make_current(qts);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_attrib_depth(qts, 2);
    gl_reset();
    gl_make_current(qts);
    assert_attrib_depth(qts, 0);
    kona_command(qts, 0xc9, NULL, 0);
    gl_reset();
    gl_make_current(qts);
    assert_attrib_depth(qts, 0);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_attrib_depth(qts, 1);
}

/* A refused push must not add an entry above a supported saved group. */
static void test_push_attrib_refused(gconstpointer opaque)
{
    uint32_t mask = GPOINTER_TO_UINT(opaque);
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    kona_command_f(s, 0x6b, (float[]){ 0.125, 0.25, 0.5, 1 }, 4);
    kona_command(s, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_reply_posted(s);
    kona_command_f(s, 0x6b, (float[]){ 1, 0, 0, 1 }, 4);
    prepare_refused_reply(s);
    kona_command(s, 0xc9, (uint32_t[]){ mask | 1 }, 1);
    assert_reply_unchanged(s);
    assert_attrib_depth(s, 1);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_posted(s);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, fbits(0.125));
    assert_attrib_depth(s, 0);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "sgi-kona: glPushAttrib"));
}

/* The outgoing page exists, but reads and pushes need a current context. */
static void test_reply_without_context_refused(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    kona_command_kicked(s, 0x1fc, (uint32_t[]){ 1 }, 1);
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]){ 0, 0, 1, 1, 0x1908, 0x1401, 0 }, 7);
    assert_reply_unchanged(s);
    kona_command(s, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_reply_unchanged(s);
    kona_command(s, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_unchanged(s);
    qtest_quit(s);
}

/*
 * glPushAttrib's 0xc9 of the current, polygon and depth bits: the reply
 * from byte 4 is the current section's 28 words, the colour first and
 * the normal at words 5 to 7, then the polygon section's 4 words with
 * the offset factor in word 1 and the units in word 3; the depth bit has
 * no reader and adds nothing. After the colour, offset and depth function
 * change, 0xc8 0 brings all three back, as glGet answers. A model that
 * kept no copy answers the changed values.
 */
static void test_push_and_pop_attrib(void)
{
    const float colour[4] = { 0.125, 0.25, 0.375, 0.5 };
    const uint64_t offset = RETURN_PAGE + 4 + 4 * 28;

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 0);
    kona_command_f(qts, 0x6b, colour, 4);
    kona_command_f(qts, 0x78, (float[]){ 0, 0.6, 0.8 }, 3);
    kona_command_f(qts, 0x168, (float[]){ 2, 3 }, 2);
    kona_command(qts, 0xce, (uint32_t[]){ 0x203 }, 1);

    kona_command(qts, 0xc9, (uint32_t[]){ 0x1 | 0x8 | 0x100 }, 1);
    assert_reply_posted(qts);
    for (int k = 0; k < 4; k++) {
        g_assert_cmphex(guest_word(qts, RETURN_PAGE + 4 + 4 * k), ==,
                        fbits(colour[k]));
    }
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 4 + 4 * 5), ==, fbits(0));
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 4 + 4 * 6), ==,
                    fbits(0.6));
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 4 + 4 * 7), ==,
                    fbits(0.8));
    g_assert_cmphex(guest_word(qts, offset + 4), ==, fbits(2));
    g_assert_cmphex(guest_word(qts, offset + 12), ==, fbits(3));

    kona_command_f(qts, 0x6b, (float[]){ 1, 1, 1, 1 }, 4);
    kona_command_f(qts, 0x168, (float[]){ 0, 0 }, 2);
    kona_command(qts, 0xce, (uint32_t[]){ 0x207 }, 1);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);

    kona_command(qts, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_posted(qts);
    for (int k = 0; k < 4; k++) {
        g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8 + 4 * k), ==,
                        fbits(colour[k]));
    }
    kona_command(qts, 0x4c, (uint32_t[]){ 0x8038 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, fbits(2));
    kona_command(qts, 0x4c, (uint32_t[]){ 0xb74 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 0x203);
}

/*
 * A pushed attribute stack crosses a migration: the source pushes the
 * current bit and changes the colour, and the destination's 0xc8 0
 * brings the source's colour back.
 */
static void test_attrib_stack_migrates(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-3d-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    gl_make_current(from);
    kona_command_f(from, 0x6b, (float[]){ 0.25, 0.5, 0.75, 1 }, 4);
    kona_command(from, 0xc9, (uint32_t[]){ 0x1 }, 1);
    kona_command_f(from, 0x6b, (float[]){ 1, 1, 1, 1 }, 4);
    assert_attrib_depth(from, 1);
    sgi_migrate_stopped(from, uri);
    assert_attrib_depth(to, 1);

    qtest_memset(to, RETURN_PAGE, 0, 64);
    kona_command(to, 0xc8, (uint32_t[]){ 0 }, 1);
    kona_command(to, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_posted(to);
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 8), ==, fbits(0.25));
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 12), ==, fbits(0.5));
    assert_attrib_depth(to, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void assert_native_depth(QTestState *s, uint32_t name,
                                const uint32_t *words, unsigned count)
{
    qtest_memset(s, RETURN_PAGE, 0, 4);
    qtest_memset(s, RETURN_PAGE + 4, 0xa5, 64);
    kona_command(s, 0x4c, &name, 1);
    assert_reply_posted(s);
    for (unsigned k = 0; k < count; k++) {
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 8 + 4 * k), ==, words[k]);
    }
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8 + 4 * count), ==, 0xa5a5a5a5);
}

static void test_native_depth_state(void)
{
    static const uint32_t range[] = { 0x3fe00000, 1, 0x3fd00000, 1 };
    static const uint32_t clear[] = { 0x3fe00000, 3 };
    static const uint32_t zero_one[] = { 0, 0, 0x3ff00000, 0 };

    gl_reset();
    gl_make_current(qts);
    assert_native_depth(qts, 0xb70, zero_one, 4);
    assert_native_depth(qts, 0xb73, zero_one + 2, 2);
    kona_command(qts, 0xcf, range, 4);
    kona_command(qts, 0x1f, clear, 2);
    assert_native_depth(qts, 0xb70, range, 4);
    assert_native_depth(qts, 0xb73, clear, 2);
    kona_command(qts, 0xc9, (uint32_t[]){ 0x800 | 0x100 | 0x20 }, 1);
    assert_reply_posted(qts);
    kona_command_d(qts, 0xcf, (double[]){ -2, 3 }, 2);
    kona_command_d(qts, 0x1f, (double[]){ -1 }, 1);
    assert_native_depth(qts, 0xb70, zero_one, 4);
    assert_native_depth(qts, 0xb73, zero_one, 2);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_depth(qts, 0xb70, range, 4);
    assert_native_depth(qts, 0xb73, clear, 2);
    kona_command(qts, 0xcf,
                 (uint32_t[]){ 0x7ff00000, 0, 0x3fe00000, 0 }, 4);
    kona_command(qts, 0x1f, (uint32_t[]){ 0x7ff80000, 1 }, 2);
    assert_native_depth(qts, 0xb70, range, 4);
    assert_native_depth(qts, 0xb73, clear, 2);
    kona_command(qts, 0xcf,
                 (uint32_t[]){ 0x80000000, 0, 0x3ff00000, 0 }, 4);
    assert_native_depth(qts, 0xb70,
                        (uint32_t[]){ 0x80000000, 0, 0x3ff00000, 0 }, 4);
    gl_reset();
    gl_make_current(qts);
    assert_native_depth(qts, 0xb70, zero_one, 4);
    assert_native_depth(qts, 0xb73, zero_one + 2, 2);
}

static void test_native_state_migrates(void)
{
    static const uint32_t range[] = { 0x3fd00000, 1, 0x3fe00000, 1 };
    static const uint32_t clear[] = { 0x3fe00000, 3 };
    g_autofree char *tmpfs = NULL, *uri = NULL, *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-return-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    gl_make_current(from);
    gl_window_cleared(from, 0.25, 0.5, 0.75);
    kona_command(from, 0xcf, range, 4);
    kona_command(from, 0x1f, clear, 2);
    kona_command(from, 0x37, (uint32_t[]){ 0x400 }, 1);
    kona_command(from, 0xc9, (uint32_t[]){ 0x800 | 0x100 | 0x20 }, 1);
    assert_reply_posted(from);
    kona_command_d(from, 0xcf, (double[]){ 0.75, 0.125 }, 2);
    kona_command_d(from, 0x1f, (double[]){ 0.25 }, 1);
    kona_command(from, 0x37, (uint32_t[]){ 0 }, 1);
    sgi_migrate_stopped(from, uri);
    assert_source_refused(to);
    assert_native_depth(to, 0xb70,
                        (uint32_t[]){ 0x3fe80000, 0, 0x3fc00000, 0 }, 4);
    assert_native_depth(to, 0xb73, (uint32_t[]){ 0x3fd00000, 0 }, 2);
    kona_command(to, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_depth(to, 0xb70, range, 4);
    assert_native_depth(to, 0xb73, clear, 2);
    assert_read_buffer(to, 0x400);
    assert_read_colour(to, 0x4080bf);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/* RGB setters replace alpha with one in the current-color reply. */
static void test_current_colour_rgb(void)
{
    gl_reset();
    gl_make_current(qts);
    kona_command_f(qts, 0x6b, (float[]){ 1, 0, 0, 0.125 }, 4);
    kona_command_f(qts, 0x63, (float[]){ 0.25, 0.5, 0.75 }, 3);
    kona_command(qts, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, fbits(0.25));
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 12), ==, fbits(0.5));
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 16), ==, fbits(0.75));
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 20), ==, fbits(1));
}

static void return_context_deactivate(QTestState *s)
{
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, 0x1fc);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, 0);
}

static void return_context_restore(QTestState *s, uint64_t save)
{
    kona_command_kicked(s, 0x1f9,
                        (uint32_t[]){ guest_word(s, save),
                                      physical_page_word(save),
                                      tagged_offset_word(save),
                                      0xedededed }, 4);
    kona_command_kicked(s, 0x1ed, (uint32_t[]){ 0, 0x9000, 0 }, 3);
    kona_command_kicked(s, guest_word(s, save + 8), (uint32_t[]){ 0 }, 1);
}

static void attrib_context_fresh(QTestState *s, uint64_t save)
{
    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ physical_page_word(save),
                                      tagged_offset_word(save), 0x10000 }, 3);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
}

static void test_native_attrib_depth_contexts(void)
{
    const uint64_t save_b = SAVE_AREA + 0x20000;

    gl_reset();
    gl_make_current(qts);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_attrib_depth(qts, 2);
    return_context_deactivate(qts);
    attrib_context_fresh(qts, save_b);
    /* This context has no drawable or visual. */
    assert_attrib_depth(qts, 0);
    kona_command(qts, 0xc9, (uint32_t[]){ 1 }, 1);
    assert_attrib_depth(qts, 1);
    return_context_deactivate(qts);
    return_context_restore(qts, SAVE_AREA);
    assert_attrib_depth(qts, 2);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    assert_attrib_depth(qts, 2);
    place_read_window(qts, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
    assert_attrib_depth(qts, 2);
    return_context_deactivate(qts);
    return_context_restore(qts, save_b);
    assert_attrib_depth(qts, 1);
    return_context_deactivate(qts);
    return_context_restore(qts, SAVE_AREA);
    assert_attrib_depth(qts, 2);
    return_context_deactivate(qts);
    attrib_context_fresh(qts, SAVE_AREA);
    assert_attrib_depth(qts, 0);
}

static void attrib_migration_pop_check(QTestState *s)
{
    const uint64_t save_b = SAVE_AREA + 0x20000;

    assert_attrib_depth(s, 1);
    return_context_deactivate(s);
    return_context_restore(s, SAVE_AREA);
    assert_attrib_depth(s, 2);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_attrib_depth(s, 1);
    kona_command(s, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, fbits(0.5));
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_attrib_depth(s, 0);
    kona_command(s, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, fbits(0.25));
    return_context_deactivate(s);
    return_context_restore(s, save_b);
    assert_attrib_depth(s, 1);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_attrib_depth(s, 0);
    kona_command(s, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, fbits(0.75));
}

static void test_native_attrib_depth_migrates(gconstpointer opaque)
{
    bool empty = GPOINTER_TO_INT(opaque);
    const uint64_t save_b = SAVE_AREA + 0x20000;
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-kona-attrib-depth-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_init(args);
    gl_make_current(from);
    if (!empty) {
        kona_command_f(from, 0x6b, (float[]){ 0.25, 0, 0, 1 }, 4);
        kona_command(from, 0xc9, (uint32_t[]){ 1 }, 1);
        kona_command_f(from, 0x6b, (float[]){ 0.5, 0, 0, 1 }, 4);
        kona_command(from, 0xc9, (uint32_t[]){ 1 }, 1);
        kona_command_f(from, 0x6b, (float[]){ 1, 0, 0, 1 }, 4);
        assert_attrib_depth(from, 2);
        return_context_deactivate(from);
        attrib_context_fresh(from, save_b);
        kona_command_f(from, 0x6b, (float[]){ 0.75, 0, 0, 1 }, 4);
        kona_command(from, 0xc9, (uint32_t[]){ 1 }, 1);
        kona_command_f(from, 0x6b, (float[]){ 0, 0, 0, 1 }, 4);
        assert_attrib_depth(from, 1);
    } else {
        assert_attrib_depth(from, 0);
    }
    sgi_migrate_stopped(from, uri);
    if (empty) {
        assert_attrib_depth(to, 0);
        assert_attrib_depth(from, 0);
    } else {
        attrib_migration_pop_check(to);
        attrib_migration_pop_check(from);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_native_attrib_depth_refused(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);

    if (kind == 0) {
        gl_make_current(s);
        return_context_deactivate(s);
    } else {
        kona_command_kicked(s, 0x1fd,
                            (uint32_t[]){ physical_page_word(SAVE_AREA),
                                          tagged_offset_word(SAVE_AREA),
                                          0x10000 }, 3);
        if (kind == 2) {
            /* Outside this fixture's 64 MiB RAM. */
            kona_command_kicked(s, 0x1f2,
                                (uint32_t[]){ physical_page_word(0x08000000),
                                              tagged_offset_word(0x08000000),
                                              0x4000 }, 3);
        }
    }
    prepare_refused_reply(s);
    kona_command(s, 0x4c, (uint32_t[]){ 0xbb0 }, 1);
    assert_reply_unchanged(s);
    /* Restore an ordinary active page for unrelated refusal controls. */
    if (kind == 0) {
        return_context_restore(s, SAVE_AREA);
    }
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    assert_attrib_depth(s, 0);
    for (unsigned k = 0; k < 3; k++) {
        const uint32_t names[] = { 0xd33, 0x2a00, 0x8148 };

        prepare_refused_reply(s);
        kona_command(s, 0x4c, &names[k], 1);
        assert_reply_unchanged(s);
    }
    qtest_quit(s);
}

/* Two contexts share a page, keeping their query and source state separate. */
static void test_return_context_isolation(void)
{
    static const uint32_t range_a[] = { 0x3fd00000, 1, 0x3fe00000, 1 };
    static const uint32_t defaults[] = { 0, 0, 0x3ff00000, 0 };
    static const uint32_t config[10] = { 0, 12 };
    const uint64_t save_b = SAVE_AREA + 0x20000;

    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    kona_command(s, 0xcf, range_a, 4);
    kona_command_f(s, 0x6b, (float[]){ 0.25, 0.5, 0.75, 0.125 }, 4);
    kona_command(s, 0x37, (uint32_t[]){ 0x400 }, 1);
    return_context_deactivate(s);
    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ physical_page_word(save_b),
                                      tagged_offset_word(save_b), 0x10000 }, 3);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    kona_command(s, 1, config, 10);
    gl_window_cleared(s, 1, 0, 0);
    assert_native_depth(s, 0xb70, defaults, 4);
    assert_read_buffer(s, 0x404);
    kona_command(s, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_posted(s);
    for (unsigned k = 0; k < 4; k++) {
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 8 + 4 * k), ==, fbits(1));
    }
    kona_command(s, 4, (uint32_t[]){ 0, 8, 0, 0, 0, 0, 0, 0, 0, 0 }, 10);
    kona_command(s, 0x114, (uint32_t[]){ 0xabcdef01 }, 1);
    assert_source_refused(s);
    return_context_deactivate(s);
    return_context_restore(s, SAVE_AREA);
    assert_native_depth(s, 0xb70, range_a, 4);
    assert_read_buffer(s, 0x400);
    kona_command(s, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_posted(s);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, fbits(0.25));
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 20), ==, fbits(0.125));
    /* Framebuffer pixels are shared even though source eligibility is not. */
    assert_read_colour(s, 0xff0000);
    return_context_deactivate(s);
    return_context_restore(s, save_b);
    assert_native_depth(s, 0xb70, defaults, 4);
    assert_source_refused(s);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
}

/* Invalid records, unknown binding and partial packets survive migration. */
static void test_return_source_migrates(gconstpointer opaque)
{
    unsigned value = GPOINTER_TO_UINT(opaque);
    unsigned token = value & 0xfff;
    bool partial = value & 0x1000;
    unsigned count = token == 0x114 ? 1 : 10;
    unsigned prefix = partial ? (token == 0x114 ? 0 : 5) : count;
    static const uint32_t valid[] = { 0, 12, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t invalid[] = { 0x100, 12, 0, 0, 0, 0, 0, 0, 0, 0 };
    g_autofree char *tmpfs = NULL, *uri = NULL, *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-source-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    gl_make_current(from);
    gl_window_cleared(from, 0, 0, 1);
    kona_command(from, 4, valid, 10);
    kona_command(from, token, invalid, prefix);
    sgi_migrate_stopped(from, uri);
    for (unsigned k = prefix; k < count; k++) {
        qtest_writel(to, REG_COMMAND, invalid[k]);
    }
    assert_source_refused(to);
    kona_command(to, 1, valid, 10);
    kona_command(to, 4, valid, 10);
    kona_command(to, 0x37, (uint32_t[]){ 0x404 }, 1);
    if (token == 0x114) {
        assert_source_refused(to);
    } else {
        assert_read_buffer(to, 0x404);
        assert_read_colour(to, 0x0000ff);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/* Destroy and replacement placement retain the context's double visual. */
static void test_read_destroyed_window_replaced(gconstpointer opaque)
{
    bool migrate = GPOINTER_TO_INT(opaque);
    static const uint32_t config[10] = { 0, 12, 0, 1 };
    static const uint32_t single[10] = { 0, 12 };
    g_autofree char *tmpfs = NULL, *uri = NULL, *dest_args = NULL;
    GError *err = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL, *s = from;

    gl_make_current(from);
    kona_command(from, 1, config, 10);
    gl_window_cleared(from, 0, 0, 1);
    kona_command(from, 0xc4, (uint32_t[]){ 0x404 }, 1);
    kona_command_f(from, 0x1d, (float[]){ 1, 0, 0, 1 }, 4);
    kona_command(from, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    assert_read_buffer(from, 0x405);
    assert_read_colour(from, 0x0000ff);
    kona_command(from, 0xc6, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(from, 0x1f4, (uint32_t[]){ 1, 1 }, 2);
    kona_command_kicked(from, 0x172, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(from, 0x1e6, (uint32_t[]){ 0x800003, 0xd, 5 }, 3);
    kona_command_kicked(from, 0x1e7, (uint32_t[]){ 1 }, 1);
    qtest_clock_step(from, 20000000);
    assert_read_colour(from, 0xff0000);
    kona_command(from, 0x37, (uint32_t[]){ 0x405 }, 1);
    assert_read_buffer(from, 0x405);
    kona_command_kicked(from, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    assert_source_refused(from);
    place_read_window(from, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
    if (migrate) {
        tmpfs = g_dir_make_tmp("sn0-kona-replace-migration-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
        dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
        to = qtest_init(dest_args);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    assert_read_buffer(s, 0x405);
    assert_read_colour(s, 0x0000ff);
    kona_command(s, 0x37, (uint32_t[]){ 0x404 }, 1);
    assert_read_colour(s, 0xff0000);
    kona_command(s, 0x37, (uint32_t[]){ 0x405 }, 1);
    kona_command(s, 1, single, 10);
    assert_source_refused(s);
    kona_command(s, 1, config, 10);
    assert_read_buffer(s, 0x405);
    assert_read_colour(s, 0x0000ff);
    if (to) {
        qtest_quit(to);
        g_rmdir(tmpfs);
    }
    qtest_quit(from);
}

/* Replacement preserves refusal markers and native state across retirement. */
static void test_read_retired_state(gconstpointer opaque)
{
    unsigned token = GPOINTER_TO_UINT(opaque);
    static const uint32_t config[10] = { 0, 12, 0, 1 };
    static const uint32_t bad[10] = { 0, 7, 0, 1 };
    static const uint32_t range[] = { 0x3fd00000, 1, 0x3fe00000, 1 };
    g_autofree char *tmpfs = NULL, *uri = NULL, *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-retired-state-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    gl_make_current(from);
    kona_command(from, 1, config, 10);
    gl_window_cleared(from, 0, 0, 1);
    kona_command(from, 0xcf, range, 4);
    kona_command(from, 0x37, (uint32_t[]){ 0x400 }, 1);
    kona_command(from, token, bad, token == 0x114 ? 1 : 10);
    kona_command_kicked(from, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    assert_source_refused(from);
    sgi_migrate_stopped(from, uri);
    assert_source_refused(to);
    place_read_window(to, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
    assert_source_refused(to);
    assert_native_depth(to, 0xb70, range, 4);
    kona_command(to, 1, config, 10);
    kona_command(to, 4, config, 10);
    if (token == 0x114) {
        assert_source_refused(to);
    } else {
        assert_read_buffer(to, 0x400);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/* Geometry updates to a placed window must retain an odd swap's bank roles. */
static void test_read_placed_move_keeps_swap(void)
{
    static const uint32_t config[10] = { 0, 12, 0, 1 };

    gl_reset();
    gl_make_current(qts);
    kona_command(qts, 1, config, 10);
    gl_window_cleared(qts, 0, 0, 1);
    kona_command(qts, 0xc4, (uint32_t[]){ 0x404 }, 1);
    kona_command_f(qts, 0x1d, (float[]){ 1, 0, 0, 1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    kona_command(qts, 0xc6, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(qts, 0x1f4, (uint32_t[]){ 1, 1 }, 2);
    kona_command_kicked(qts, 0x172, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(qts, 0x1e6, (uint32_t[]){ 0x800003, 0xd, 5 }, 3);
    kona_command_kicked(qts, 0x1e7, (uint32_t[]){ 1 }, 1);
    qtest_clock_step(qts, 20000000);
    kona_command(qts, 0x37, (uint32_t[]){ 0x404 }, 1);
    assert_read_colour(qts, 0x0000ff);
    place_read_window(qts, PROBE_X + 1, PROBE_Y + 1, PROBE_SIZE, PROBE_SIZE);
    assert_read_buffer(qts, 0x404);
    assert_read_colour(qts, 0x0000ff);
    kona_command(qts, 0x37, (uint32_t[]){ 0x405 }, 1);
    assert_read_colour(qts, 0xff0000);
}

static void test_read_single_retired_replaced(gconstpointer opaque)
{
    bool migrate = GPOINTER_TO_INT(opaque);
    static const uint32_t single[10] = { 0, 12 };
    static const uint32_t doubled[10] = { 0, 12, 0, 1 };
    g_autofree char *tmpfs = NULL, *uri = NULL, *dest_args = NULL;
    GError *err = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL, *s = from;

    gl_make_current(from);
    gl_window_cleared(from, 0, 0, 1);
    assert_read_buffer(from, 0x404);
    kona_command(from, 0x37, (uint32_t[]){ 0x400 }, 1);
    kona_command_kicked(from, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    assert_source_refused(from);
    if (migrate) {
        tmpfs = g_dir_make_tmp("sn0-kona-single-retired-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
        dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
        to = qtest_init(dest_args);
        sgi_migrate_stopped(from, uri);
        s = to;
        assert_source_refused(s);
    }
    place_read_window(s, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
    assert_read_buffer(s, 0x400);
    assert_read_colour(s, 0x0000ff);
    kona_command(s, 0x37, (uint32_t[]){ 0x405 }, 1);
    assert_source_refused(s);
    kona_command_kicked(s, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    place_read_window(s, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
    assert_source_refused(s);
    kona_command(s, 1, doubled, 10);
    kona_command_f(s, 0x1d, (float[]){ 0, 1, 0, 1 }, 4);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    assert_read_buffer(s, 0x405);
    assert_read_colour(s, 0x00ff00);
    kona_command(s, 1, single, 10);
    assert_source_refused(s);
    if (to) {
        qtest_quit(to);
        g_rmdir(tmpfs);
    }
    qtest_quit(from);
}

static void stencil_config(QTestState *s, uint32_t width)
{
    kona_command(s, 1,
                 (uint32_t[]){ 0, 12, 0, 0, 0, 0, 24, width, 0, 0 }, 10);
}

static void stencil_triangle(QTestState *s)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };

    kona_command_f(s, 0x63, (float[]){ 1, 0, 0 }, 3);
    gl_triangle_z(s, tri, 0);
}

static void assert_stencil_colour(QTestState *s, uint32_t rgb)
{
    g_autofree uint8_t *image = kona_screendump(s);
    unsigned at = ((FB_HEIGHT - 1 - (PROBE_Y + PROBE_SIZE / 2)) *
                   FB_WIDTH + PROBE_X + PROBE_SIZE / 2) * 3;

    g_assert_cmphex(image[at], ==, (rgb >> 16) & 255);
    g_assert_cmphex(image[at + 1], ==, (rgb >> 8) & 255);
    g_assert_cmphex(image[at + 2], ==, rgb & 255);
}

static void test_stencil_visual_present_and_absent(void)
{
    gl_reset();
    gl_make_current_depth(qts, 24);
    gl_window_cleared(qts, 0, 0, 1);
    stencil_config(qts, 8);
    kona_command(qts, 0x1e, (uint32_t[]){ 61 }, 1);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x400 }, 1);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb90 }, 1);
    kona_command(qts, 0xcc, (uint32_t[]){ 0x200, 0, 255 }, 3);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0x0000ff);
    stencil_config(qts, 0);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0xff0000);
    kona_command(qts, 0x1e, (uint32_t[]){ 9 }, 1);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x400 }, 1);
    stencil_config(qts, 8);
    kona_command_f(qts, 0x1d, (float[]){ 0, 0, 1, 1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    kona_command(qts, 0xcc, (uint32_t[]){ 0x202, 61, 255 }, 3);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0xff0000);
}

static void stencil_setup(QTestState *s)
{
    gl_make_current_depth(s, 24);
    gl_window_cleared(s, 0, 0, 1);
    stencil_config(s, 8);
}

static void stencil_clear(QTestState *s, uint32_t value, uint32_t mask)
{
    kona_command(s, 0x20, (uint32_t[]){ mask }, 1);
    kona_command(s, 0x1e, (uint32_t[]){ value }, 1);
    kona_command(s, 0x1a, (uint32_t[]){ 0x400 }, 1);
}

static void stencil_function(QTestState *s, uint32_t func, int32_t ref,
                             uint32_t mask)
{
    kona_command(s, 0x24, (uint32_t[]){ 0xb90 }, 1);
    kona_command(s, 0xcc, (uint32_t[]){ func, ref, mask }, 3);
}

static void stencil_operations(QTestState *s, uint32_t fail, uint32_t zfail,
                               uint32_t pass)
{
    kona_command(s, 0xcd, (uint32_t[]){ fail, zfail, pass }, 3);
}

/* Observe through EQUAL and colour, without adding a stencil reply. */
static void assert_stencil_value(QTestState *s, uint32_t value)
{
    kona_command(s, 0x25, (uint32_t[]){ 0xbc0 }, 1);
    kona_command(s, 0x25, (uint32_t[]){ 0xb71 }, 1);
    kona_command(s, 0x21, (uint32_t[]){ 1, 1, 1, 1 }, 4);
    kona_command(s, 0xc4, (uint32_t[]){ 0x404 }, 1);
    kona_command_f(s, 0x1d, (float[]){ 0, 0, 1, 1 }, 4);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    stencil_function(s, 0x202, value, 255);
    stencil_operations(s, 0x1e00, 0x1e00, 0x1e00);
    stencil_triangle(s);
    assert_stencil_colour(s, 0xff0000);
}

static void test_stencil_comparison_order_and_mask(void)
{
    const uint32_t funcs[] = { 0x201, 0x204, 0x202, 0x202 };
    const int32_t refs[] = { 2, 9, -1, 300 };
    const uint32_t stored[] = { 5, 5, 0, 255 };
    const uint32_t expected[] = { 2, 9, 0, 255 };

    gl_reset();
    stencil_setup(qts);
    for (unsigned i = 0; i < G_N_ELEMENTS(funcs); i++) {
        stencil_clear(qts, stored[i], 255);
        stencil_function(qts, funcs[i], refs[i], 255);
        stencil_operations(qts, 0x150a, 0, 0x1e01);
        stencil_triangle(qts);
        assert_stencil_value(qts, expected[i]);
    }
    stencil_clear(qts, 0xa5, 255);
    kona_command(qts, 0x20, (uint32_t[]){ 0xf0 }, 1);
    stencil_function(qts, 0x202, 0x35, 0x0f);
    stencil_operations(qts, 0, 0, 0x1e01);
    stencil_triangle(qts);
    assert_stencil_value(qts, 0x35);
}

static void test_stencil_operation_range(void)
{
    static const struct {
        uint32_t initial, op;
        int32_t ref;
        uint32_t expected;
    } cases[] = {
        { 27, 0, 0, 0 }, { 27, 0x1e01, 91, 91 },
        { 0xa5, 0x150a, 0, 0x5a },
        { 255, 0x1e02, 0, 255 }, { 0, 0x1e03, 0, 0 },
        { 1, 0x1e02, 0, 2 }, { 1, 0x1e03, 0, 0 },
        { 11, 0x1e01, -1, 0 }, { 11, 0x1e01, 300, 255 },
    };

    gl_reset();
    stencil_setup(qts);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        stencil_clear(qts, cases[i].initial, 255);
        stencil_function(qts, 0x207, cases[i].ref, 255);
        stencil_operations(qts, 0x1e00, 0x1e00, cases[i].op);
        stencil_triangle(qts);
        assert_stencil_value(qts, cases[i].expected);
    }
}

static uint32_t stencil_colour_or_depth(QTestState *s, unsigned plane)
{
    QDict *reply;
    uint32_t value;

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane','value':%u}}", plane);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-x','value':%u}}", PROBE_X + PROBE_SIZE / 2);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-y','value':%u}}", PROBE_Y + PROBE_SIZE / 2);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane-word'}}");
    value = qdict_get_int(reply, "return");
    qobject_unref(reply);
    return value;
}

static void test_stencil_outcome_operations(void)
{
    gl_reset();
    stencil_setup(qts);
    for (unsigned outcome = 0; outcome < 5; outcome++) {
        stencil_config(qts, 8);
        if (outcome == 4) {
            kona_command(qts, 1,
                         (uint32_t[]){ 0, 12, 0, 0, 0, 0, 0, 8, 0, 0 }, 10);
        }
        kona_command_f(qts, 0x1d, (float[]){ 0, 0, 1, 1 }, 4);
        kona_command(qts, 0x1a, (uint32_t[]){ 0x4100 }, 1);
        stencil_clear(qts, 40, 255);
        stencil_function(qts, outcome == 0 ? 0x200 : 0x207, 99, 255);
        kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);
        kona_command(qts, 0xce,
                     (uint32_t[]){ outcome == 1 ? 0x200 : 0x207 }, 1);
        if (outcome == 3) {
            kona_command(qts, 0x25, (uint32_t[]){ 0xb71 }, 1);
        }
        stencil_operations(qts, 0, 0x1e02, 0x1e03);
        stencil_triangle(qts);
        assert_stencil_colour(qts, outcome < 2 ? 0x0000ff : 0xff0000);
        g_assert_cmphex(stencil_colour_or_depth(qts, 5), ==,
                        outcome == 2 ? 0x800000 : 0xffffff);
        assert_stencil_value(qts, outcome == 0 ? 0 : outcome == 1 ? 41 : 39);
    }
}


static void test_stencil_only_clear(void)
{
    uint32_t before[3];

    gl_reset();
    stencil_setup(qts);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x100 }, 1);
    before[0] = stencil_colour_or_depth(qts, 0);
    before[1] = stencil_colour_or_depth(qts, 1);
    before[2] = stencil_colour_or_depth(qts, 5);
    stencil_clear(qts, 0xa5, 255);
    stencil_function(qts, 0x200, 0, 255);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);
    kona_command(qts, 0xce, (uint32_t[]){ 0x200 }, 1);
    stencil_clear(qts, 0x13c, 0x0f);
    assert_stencil_colour(qts, 0x0000ff);
    g_assert_cmphex(stencil_colour_or_depth(qts, 0), ==, before[0]);
    g_assert_cmphex(stencil_colour_or_depth(qts, 1), ==, before[1]);
    g_assert_cmphex(stencil_colour_or_depth(qts, 5), ==, before[2]);
    assert_stencil_value(qts, 0xac);
}

static void test_stencil_alpha_and_disabled_tests(void)
{
    gl_reset();
    stencil_setup(qts);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x100 }, 1);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);
    kona_command(qts, 0xce, (uint32_t[]){ 0x207 }, 1);
    stencil_clear(qts, 19, 255);
    stencil_function(qts, 0x207, 80, 255);
    stencil_operations(qts, 0, 0, 0x1e01);
    kona_command(qts, 0x24, (uint32_t[]){ 0xbc0 }, 1);
    kona_command(qts, 0xca, (uint32_t[]){ 0x200, F0 }, 2);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0x0000ff);
    g_assert_cmphex(stencil_colour_or_depth(qts, 5), ==, 0xffffff);
    assert_stencil_value(qts, 19);
    stencil_operations(qts, 0, 0, 0x1e01);
    kona_command(qts, 0x25, (uint32_t[]){ 0xb90 }, 1);
    stencil_triangle(qts);
    assert_stencil_value(qts, 19);
}

static void test_stencil_masks_and_colour_targets(void)
{
    gl_reset();
    stencil_setup(qts);
    kona_command(qts, 1,
                 (uint32_t[]){ 0, 12, 0, 1, 0, 0, 24, 8, 0, 0 }, 10);
    for (unsigned target = 0; target < 2; target++) {
        stencil_clear(qts, 10, 255);
        stencil_function(qts, 0x207, 0, 255);
        stencil_operations(qts, 0x1e00, 0x1e00, 0x1e02);
        kona_command(qts, 0x21, (uint32_t[]){ 0, 0, 0, 0 }, 4);
        kona_command(qts, 0x22, (uint32_t[]){ 0 }, 1);
        kona_command(qts, 0xc4, (uint32_t[]){ target ? 0 : 0x408 }, 1);
        stencil_triangle(qts);
        assert_stencil_value(qts, 11);
    }
    kona_command(qts, 0xc4, (uint32_t[]){ 0 }, 1);
    stencil_clear(qts, 71, 255);
    assert_stencil_value(qts, 71);
    kona_command(qts, 0xc6, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(qts, 0x1f4, (uint32_t[]){ 1, 1 }, 2);
    kona_command_kicked(qts, 0x172, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(qts, 0x1e6, (uint32_t[]){ 0x800003, 0xd, 5 }, 3);
    kona_command_kicked(qts, 0x1e7, (uint32_t[]){ 1 }, 1);
    qtest_clock_step(qts, 20000000);
    assert_stencil_value(qts, 71);
}

static void test_stencil_incomplete_or_invalid_visual_refused(void)
{
    static const struct { unsigned field; uint32_t value; } bad[] = {
        { 7, 4 }, { 4, 1 }, { 9, 1 }, { 1, 7 }, { 6, 33 },
        { 0, 0x100 }, { 2, 8 }, { 3, 2 }, { 5, 1 }, { 8, 1 },
    };

    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ physical_page_word(SAVE_AREA),
                                      tagged_offset_word(SAVE_AREA),
                                      0x10000 }, 3);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    gl_window_cleared(s, 0, 0, 1);
    stencil_function(s, 0x207, 99, 255);
    stencil_triangle(s);
    assert_stencil_colour(s, 0x0000ff);
    kona_command_f(s, 0x1d, (float[]){ 0, 1, 0, 1 }, 4);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4500 }, 1);
    assert_stencil_colour(s, 0x0000ff);
    for (unsigned width = 0; width < 2; width++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(bad); i++) {
            uint32_t config[] = { 0, 12, 0, 0, 0, 0, 24, width ? 8 : 0, 0, 0 };
            uint32_t depth_before;

            stencil_config(s, 8);
            stencil_clear(s, 43, 255);
            kona_command_f(s, 0x1d, (float[]){ 0, 0, 1, 1 }, 4);
            kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
            depth_before = stencil_colour_or_depth(s, 5);
            g_test_message("invalid visual width %u field %u", width,
                           bad[i].field);
            config[bad[i].field] = bad[i].value;
            kona_command(s, 1, config, 10);
            kona_command(s, 0xc4, (uint32_t[]){ 0x404 }, 1);
            stencil_function(s, 0x207, 99, 255);
            stencil_operations(s, 0x1e01, 0x1e01, 0x1e01);
            stencil_triangle(s);
            assert_stencil_colour(s, 0x0000ff);
            kona_command_f(s, 0x1d, (float[]){ 0, 1, 0, 1 }, 4);
            kona_command(s, 0x1e, (uint32_t[]){ 99 }, 1);
            kona_command(s, 0x1a, (uint32_t[]){ 0x4500 }, 1);
            assert_stencil_colour(s, 0x0000ff);
            g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, depth_before);
            return_context_deactivate(s);
            return_context_restore(s, SAVE_AREA);
            kona_command_kicked(s, 0x1f8, (uint32_t[]){ 0x103 }, 1);
            place_read_window(s, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
            kona_command(s, 0xc4, (uint32_t[]){ 0x404 }, 1);
            stencil_triangle(s);
            assert_stencil_colour(s, 0x0000ff);
            kona_command(s, 0x1a, (uint32_t[]){ 0x4500 }, 1);
            assert_stencil_colour(s, 0x0000ff);
            g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, depth_before);
            stencil_config(s, 8);
            assert_stencil_value(s, 43);
        }
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "stencil draw of context"),
                     ==, 41);
    g_assert_cmpuint(count_occurrences(found, "stencil clear refused"), ==, 41);
}

static void test_stencil_clear_scissor_and_clip(void)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    stencil_setup(qts);
    stencil_clear(qts, 7, 255);
    kona_command(qts, 0xc, (uint32_t[]){ 60, 57, 9, 12 }, 4);
    kona_command(qts, 0x24, (uint32_t[]){ 0xc11 }, 1);
    kona_command(qts, 0xe3, (uint32_t[]){ 0, 0, 1, 1 }, 4);
    stencil_clear(qts, 83, 255);
    kona_command(qts, 0x25, (uint32_t[]){ 0xc11 }, 1);
    kona_command(qts, 0xe3, (uint32_t[]){ 0, 0, PROBE_SIZE, PROBE_SIZE }, 4);
    assert_stencil_value(qts, 83);
    expected_clear(e, 0x0000ff);
    expected_object(e, mat_identity(), mat_identity(), tri, 3, 0xff0000);
    for (unsigned y = 0; y < PROBE_SIZE; y++) {
        for (unsigned x = 0; x < PROBE_SIZE; x++) {
            if (x < 60 || x >= 69 || y < 57 || y >= 69) {
                unsigned at = ((PROBE_SIZE - 1 - y) * PROBE_SIZE + x) * 3;

                e->rgb[at] = e->rgb[at + 1] = 0;
                e->rgb[at + 2] = 255;
            }
        }
    }
    assert_window_matches(qts, e);
    /* Scissor wholly outside the placed window cannot affect its pixels. */
    kona_command(qts, 0xc, (uint32_t[]){ 150, 5, 11, 17 }, 4);
    kona_command(qts, 0x24, (uint32_t[]){ 0xc11 }, 1);
    stencil_clear(qts, 21, 255);
    kona_command(qts, 0x25, (uint32_t[]){ 0xc11 }, 1);
    assert_stencil_value(qts, 83);
}

static void test_stencil_context_and_attrib(void)
{
    const uint64_t save_b = SAVE_AREA + 0x20000;

    gl_reset();
    stencil_setup(qts);
    stencil_clear(qts, 29, 255);
    stencil_function(qts, 0x202, 29, 255);
    stencil_operations(qts, 0x1e00, 0x1e00, 0x1e00);
    kona_command(qts, 0xc9, (uint32_t[]){ 0x400 }, 1);
    stencil_function(qts, 0x200, 1, 15);
    stencil_clear(qts, 31, 255);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0x0000ff);
    assert_stencil_value(qts, 31);
    stencil_function(qts, 0x200, 0, 255);
    return_context_deactivate(qts);
    kona_command_kicked(qts, 0x1fd,
                        (uint32_t[]){ physical_page_word(save_b),
                                      tagged_offset_word(save_b), 0x10000 }, 3);
    kona_command_kicked(qts, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    stencil_config(qts, 0);
    gl_window_cleared(qts, 0, 0, 1);
    stencil_function(qts, 0x200, 0, 255);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0xff0000);
    return_context_deactivate(qts);
    return_context_restore(qts, SAVE_AREA);
    kona_command_f(qts, 0x1d, (float[]){ 0, 0, 1, 1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0x0000ff);
    assert_stencil_value(qts, 31);
    kona_command(qts, 0xc9, (uint32_t[]){ 0x2000 }, 1);
    kona_command(qts, 0x25, (uint32_t[]){ 0xb90 }, 1);
    kona_command(qts, 0xcc, (uint32_t[]){ 0x207, 88, 255 }, 3);
    stencil_operations(qts, 0x1e00, 0x1e00, 0x1e01);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    stencil_triangle(qts);
    assert_stencil_value(qts, 88);
}

static void test_stencil_reset_and_partial_packets(void)
{
    gl_reset();
    stencil_setup(qts);
    stencil_clear(qts, 71, 255);
    stencil_function(qts, 0x200, 88, 15);
    kona_command(qts, 0xcc, (uint32_t[]){ 0x207, 0x1a }, 2);
    gl_reset();
    stencil_setup(qts);
    kona_command(qts, 0xcc, (uint32_t[]){ 0x207, 88, 255 }, 3);
    stencil_operations(qts, 0x1e00, 0x1e00, 0x1e01);
    stencil_triangle(qts);
    assert_stencil_value(qts, 0);
    kona_command(qts, 0x1a, NULL, 0);
    gl_reset();
    stencil_setup(qts);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb90 }, 1);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0xff0000);
    assert_stencil_value(qts, 0);
    stencil_function(qts, 0x207, 0x1a, 255);
    stencil_operations(qts, 0x1e00, 0x1e00, 0x1e01);
    stencil_triangle(qts);
    assert_stencil_value(qts, 0x1a);
}

static void test_stencil_state_and_plane_migrate(void)
{
    g_autofree char *dir = NULL, *uri = NULL, *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-kona-stencil-migrate-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_init(dest_args);
    stencil_setup(from);
    stencil_clear(from, 0xa3, 255);
    kona_command(from, 0x20, (uint32_t[]){ 15 }, 1);
    stencil_operations(from, 0, 0x1e03, 0x1e02);
    stencil_function(from, 0x207, 2, 255);
    kona_command(from, 0xc9, (uint32_t[]){ 0x400 }, 1);
    kona_command(from, 0xcc, (uint32_t[]){ 0x207, 0x1a }, 2);
    sgi_migrate_stopped(from, uri);
    for (unsigned i = 0; i < 2; i++) {
        QTestState *s = i ? to : from;

        qtest_writel(s, REG_COMMAND, 255);
        stencil_triangle(s);
        assert_stencil_value(s, 0xa4);
        kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
        stencil_triangle(s);
        assert_stencil_value(s, 0xa5);
    }
    assert_source_refused(to);
    return_context_deactivate(to);
    return_context_restore(to, SAVE_AREA);
    assert_source_refused(to);
    assert_stencil_value(to, 0xa5);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_stencil_visual_readback_remains_refused(void)
{
    gl_reset();
    stencil_setup(qts);
    stencil_clear(qts, 53, 255);
    assert_stencil_value(qts, 53);
    assert_source_refused(qts);
    return_context_deactivate(qts);
    return_context_restore(qts, SAVE_AREA);
    assert_source_refused(qts);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    place_read_window(qts, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
    assert_source_refused(qts);
    assert_stencil_value(qts, 53);
    stencil_config(qts, 0);
    kona_command_f(qts, 0x1d, (float[]){ 1, 0, 0, 1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    assert_read_colour(qts, 0xff0000);
}

static void test_stencil_partial_visual_migration(void)
{
    static const struct { uint32_t before, after; } cases[] = {
        { 8, 0 }, { 0, 8 }, { 8, 4 }, { 4, 8 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *dir = NULL, *uri = NULL, *dest_args = NULL;
        uint32_t config[] = { 0, 12, 0, 0, 0, 0, 24, cases[i].after, 0, 0 };
        GError *err = NULL;
        QTestState *from, *to;

        dir = g_dir_make_tmp("sn0-kona-stencil-visual-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
        from = qtest_init(MACHINE_ARGS);
        to = qtest_init(dest_args);
        stencil_setup(from);
        stencil_clear(from, 61, 255);
        stencil_function(from, 0x200, 0, 255);
        stencil_config(from, cases[i].before);
        /* The changed width is pending; the complete record still owns it. */
        kona_command(from, 1, config, 8);
        sgi_migrate_stopped(from, uri);
        for (unsigned run = 0; run < 2; run++) {
            QTestState *s = run ? to : from;

            qtest_writel(s, REG_COMMAND, config[8]);
            qtest_writel(s, REG_COMMAND, config[9]);
            stencil_triangle(s);
            if (cases[i].after == 0) {
                assert_stencil_colour(s, 0xff0000);
            } else {
                assert_stencil_colour(s, 0x0000ff);
            }
            if (cases[i].after == 0) {
                assert_read_colour(s, 0x0000ff);
            } else {
                assert_source_refused(s);
            }
            stencil_config(s, 8);
            assert_stencil_value(s, 61);
            assert_source_refused(s);
        }
        qtest_quit(to);
        qtest_quit(from);
        g_rmdir(dir);
    }
}

static void test_stencil_accumulation_clear_diagnostic(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t before[3];

    stencil_setup(s);
    kona_command(s, 0x1a, (uint32_t[]){ 0x100 }, 1);
    stencil_clear(s, 73, 255);
    before[0] = stencil_colour_or_depth(s, 0);
    before[1] = stencil_colour_or_depth(s, 1);
    before[2] = stencil_colour_or_depth(s, 5);
    kona_command(s, 0x1a, (uint32_t[]){ 0x200 }, 1);
    g_assert_cmphex(stencil_colour_or_depth(s, 0), ==, before[0]);
    g_assert_cmphex(stencil_colour_or_depth(s, 1), ==, before[1]);
    g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, before[2]);
    assert_stencil_value(s, 73);
    kona_command_f(s, 0x1d, (float[]){ 0, 1, 0, 1 }, 4);
    kona_command_d(s, 0x1f, (double[]){ 0.25 }, 1);
    kona_command(s, 0x1e, (uint32_t[]){ 91 }, 1);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4700 }, 1);
    assert_stencil_colour(s, 0x00ff00);
    g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, 0x400000);
    g_assert_cmphex(stencil_colour_or_depth(s, 1), ==, before[1]);
    assert_stencil_value(s, 91);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "glClear of buffers 0x200; only colour"));
    g_assert_nonnull(strstr(found, "glClear of buffers 0x4700; only colour"));
    g_assert_cmpuint(count_occurrences(found, "glClear of buffers"), ==, 2);
    g_assert_false(log_shows_misframing(found));
}

static void test_stencil_invalid_state_enumerants(void)
{
    static const uint32_t invalid_func[] = { 0, 0x208, UINT32_MAX };

    gl_reset();
    stencil_setup(qts);
    for (unsigned i = 0; i < G_N_ELEMENTS(invalid_func); i++) {
        static const uint32_t stored[] = { 17, 16 };
        static const uint32_t expected[] = { 17, 0 };

        for (unsigned j = 0; j < G_N_ELEMENTS(stored); j++) {
            stencil_clear(qts, stored[j], 255);
            stencil_function(qts, 0x202, 17, 255);
            stencil_operations(qts, 0, 0, 0x1e01);
            kona_command(qts, 0xcc,
                         (uint32_t[]){ invalid_func[i], 99, 0 }, 3);
            stencil_triangle(qts);
            assert_stencil_value(qts, expected[j]);
        }
    }
    for (unsigned bad = 0; bad < 3; bad++) {
        for (unsigned outcome = 0; outcome < 3; outcome++) {
            uint32_t ops[] = { 0x1e01, 0x1e01, 0x1e01 };

            stencil_clear(qts, 40, 255);
            stencil_function(qts, outcome == 0 ? 0x200 : 0x207, 99, 255);
            stencil_operations(qts, 0, 0x1e02, 0x1e03);
            kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);
            kona_command(qts, 0xce,
                         (uint32_t[]){ outcome == 1 ? 0x200 : 0x207 }, 1);
            ops[bad] = 0x1e06;
            kona_command(qts, 0xcd, ops, 3);
            stencil_triangle(qts);
            assert_stencil_value(qts, outcome == 0 ? 0 :
                                  outcome == 1 ? 41 : 39);
        }
    }
}

/* Destroy reason retires a window; fresh mapping resets context state. */
static void test_stencil_destroyed_context_pixels_retained(void)
{
    gl_reset();
    stencil_setup(qts);
    stencil_clear(qts, 73, 255);
    stencil_function(qts, 0x200, 12, 15);
    stencil_operations(qts, 0x1e01, 0x1e01, 0x1e01);
    kona_command(qts, 0x20, (uint32_t[]){ 15 }, 1);
    kona_command(qts, 0x1e, (uint32_t[]){ 91 }, 1);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    gl_make_current_depth(qts, 24);
    gl_window_cleared(qts, 0, 0, 1);
    stencil_config(qts, 8);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0xff0000);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb90 }, 1);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0xff0000);
    assert_stencil_value(qts, 73);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x400 }, 1);
    assert_stencil_value(qts, 0);
}

static const double depth_blend_triangle[3][3] = {
    { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
};

static void test_depth_clear_preserves_colour(void)
{
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    gl_make_current_depth(qts, 24);
    kona_command(qts, 1,
                 (uint32_t[]){ 0, 12, 0, 1, 0, 0, 24, 0, 0, 0 }, 10);
    gl_window_cleared(qts, 0, 0, 1);
    kona_command(qts, 0xc4, (uint32_t[]){ 0x404 }, 1);
    kona_command_f(qts, 0x1d, (float[]){ 1, 0, 0, 1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    kona_command(qts, 0xc4, (uint32_t[]){ 0x408 }, 1);
    kona_command_f(qts, 0x1d, (float[]){ 0, 1, 0, 1 }, 4);
    kona_command_d(qts, 0x1f, (double[]){ 0.25 }, 1);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x100 }, 1);
    g_assert_cmphex(stencil_colour_or_depth(qts, 0), ==, 0xff0000);
    g_assert_cmphex(stencil_colour_or_depth(qts, 1), ==, 0x0000ff);
    g_assert_cmphex(stencil_colour_or_depth(qts, 5), ==, 0x400000);
    expected_clear(e, 0xff0000);
    assert_window_matches(qts, e);
    kona_command(qts, 0x22, (uint32_t[]){ 0 }, 1);
    kona_command_d(qts, 0x1f, (double[]){ 0.75 }, 1);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x100 }, 1);
    g_assert_cmphex(stencil_colour_or_depth(qts, 5), ==, 0x400000);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);
    kona_command(qts, 0xce, (uint32_t[]){ 0x207 }, 1);
    kona_command(qts, 0x21, (uint32_t[]){ 0, 0, 0, 0 }, 4);
    gl_triangle_z(qts, depth_blend_triangle, 0);
    g_assert_cmphex(stencil_colour_or_depth(qts, 5), ==, 0x400000);
    g_assert_cmphex(stencil_colour_or_depth(qts, 0), ==, 0xff0000);
    g_assert_cmphex(stencil_colour_or_depth(qts, 1), ==, 0x0000ff);
}

static void test_colour_masks_preserve_channels(void)
{
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 1, 0, 1);
    kona_command(qts, 0x21, (uint32_t[]){ 0, 1, 0, 0 }, 4);
    kona_command_f(qts, 0x1d, (float[]){ 0, 1, 0, 1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    expected_clear(e, 0xffffff);
    assert_window_matches(qts, e);
    kona_command(qts, 0x21, (uint32_t[]){ 1, 0, 1, 0 }, 4);
    kona_command_f(qts, 0x63, (float[]){ 0, 0, 0 }, 3);
    gl_triangle_z(qts, depth_blend_triangle, 0);
    expected_object(e, mat_identity(), mat_identity(), depth_blend_triangle,
                    3, 0x00ff00);
    assert_window_matches(qts, e);
}

static void test_depth_plane_migration(void)
{
    g_autofree char *dir = NULL, *uri = NULL, *dest_args = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    GError *err = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-kona-depth-qualify-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_init(dest_args);
    gl_make_current_depth(from, 24);
    gl_window_cleared(from, 0, 0, 1);
    kona_command(from, 0x1a, (uint32_t[]){ 0x100 }, 1);
    kona_command(from, 0x24, (uint32_t[]){ 0xb71 }, 1);
    kona_command(from, 0xce, (uint32_t[]){ 0x201 }, 1);
    kona_command_f(from, 0x63, (float[]){ 1, 0, 0 }, 3);
    gl_triangle_z(from, depth_blend_triangle, -0.5);
    g_assert_cmphex(stencil_colour_or_depth(from, 5), ==, 0x400000);
    sgi_migrate_stopped(from, uri);
    expected_clear(e, 0x0000ff);
    expected_object(e, mat_identity(), mat_identity(), depth_blend_triangle,
                    3, 0xff0000);
    for (unsigned run = 0; run < 2; run++) {
        QTestState *s = run ? to : from;

        g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, 0x400000);
        assert_window_matches(s, e);
        kona_command_f(s, 0x63, (float[]){ 0, 1, 0 }, 3);
        gl_triangle_z(s, depth_blend_triangle, 0);
        assert_window_matches(s, e);
        g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, 0x400000);
        gl_triangle_z(s, depth_blend_triangle, -0.75);
        expected_clear(e, 0x0000ff);
        expected_object(e, mat_identity(), mat_identity(), depth_blend_triangle,
                        3, 0x00ff00);
        assert_window_matches(s, e);
        g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, 0x200000);
        expected_clear(e, 0x0000ff);
        expected_object(e, mat_identity(), mat_identity(), depth_blend_triangle,
                        3, 0xff0000);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_blend_without_alpha_plane(void)
{
    static const struct {
        uint32_t src, dst;
        float alpha;
        uint32_t rgb;
    } cases[] = {
        { 0x302, 0x303, 0, 0x0000ff },
        { 0x302, 0x303, 1, 0xff0000 },
        { 0x304, 0, 0.25, 0xff0000 },
        { 0, 0x305, 0.75, 0x000000 },
        { 0x308, 1, 0.25, 0x0000ff },
    };
    g_autofree Expected *e = g_new(Expected, 1);

    gl_reset();
    gl_make_current(qts);
    gl_window_cleared(qts, 0, 0, 1);
    kona_command(qts, 0x24, (uint32_t[]){ 0xbe2 }, 1);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        for (unsigned clear_alpha = 0; clear_alpha < 2; clear_alpha++) {
            kona_command_f(qts, 0x1d,
                           (float[]){ 0, 0, 1, clear_alpha }, 4);
            kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
            kona_command(qts, 0xcb,
                         (uint32_t[]){ cases[i].src, cases[i].dst }, 2);
            kona_command_f(qts, 0x6b,
                           (float[]){ 1, 0, 0, cases[i].alpha }, 4);
            gl_triangle_z(qts, depth_blend_triangle, 0);
            expected_clear(e, 0x0000ff);
            expected_object(e, mat_identity(), mat_identity(),
                            depth_blend_triangle, 3, cases[i].rgb);
            assert_window_matches(qts, e);
        }
    }
}

static const uint8_t hints_default[5] = { 0, 0, 0, 0, 0 };
static const uint8_t hints_a[5] = { 1, 2, 0, 1, 2 };
static const uint8_t hints_b[5] = { 2, 0, 1, 2, 1 };

static void set_hints(QTestState *s, const uint8_t modes[5])
{
    for (unsigned k = 0; k < 5; k++) {
        kona_command(s, 0xbb, (uint32_t[]){ 0xc50 + k, 0x1100 + modes[k] }, 2);
    }
}

static void assert_hints(QTestState *s, const uint8_t modes[5])
{
    for (unsigned k = 0; k < 5; k++) {
        assert_hint(s, k, modes[k]);
    }
}

static void test_native_hints_invalid(void)
{
    const uint32_t bad_modes[] = { 0, 0x1103, UINT32_MAX, 0x4c };
    const uint32_t bad_targets[] = { 0xc4f, 0xc55, 0x84ef, 0xbb };

    gl_reset();
    gl_make_current(qts);
    set_hints(qts, hints_a);
    for (unsigned k = 0; k < 5; k++) {
        for (unsigned j = 0; j < ARRAY_SIZE(bad_modes); j++) {
            kona_command(qts, 0xbb,
                         (uint32_t[]){ 0xc50 + k, bad_modes[j] }, 2);
            assert_hints(qts, hints_a);
        }
    }
    for (unsigned k = 0; k < ARRAY_SIZE(bad_targets); k++) {
        kona_command(qts, 0xbb, (uint32_t[]){ bad_targets[k], 0x1101 }, 2);
        assert_hints(qts, hints_a);
    }
    kona_command(qts, 0xbb, (uint32_t[]){ 0x1101, 0xc50 }, 2);
    kona_command(qts, 0xce, (uint32_t[]){ 0x203 }, 1);
    assert_hints(qts, hints_a);
    kona_command(qts, 0x4c, (uint32_t[]){ 0xb74 }, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 0x203);
    kona_command(qts, 0x57, (uint32_t[]){ 0 }, 1);
    set_hints(qts, hints_b);
    kona_command(qts, 0x71, (uint32_t[]){ 0 }, 1);
    assert_hints(qts, hints_a);
    return_context_deactivate(qts);
    set_hints(qts, hints_b);
    return_context_restore(qts, SAVE_AREA);
    assert_hints(qts, hints_a);
}

static void hint_push(QTestState *s, uint32_t mask)
{
    uint8_t got[0x4000 - 4], expected[0x4000 - 4];

    memset(expected, 0xa5, sizeof(expected));
    if (mask & 8) {
        const uint8_t offset[] = {
            0, 0, 0, 0, 0x40, 0, 0, 0,
            0, 0, 0, 0, 0x40, 0x40, 0, 0,
        };

        memcpy(expected, offset, sizeof(offset));
    }
    prepare_refused_reply(s);
    kona_command(s, 0xc9, &mask, 1);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 4, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
}

static void test_native_hints_attrib(void)
{
    const uint32_t other_groups[] = { 0x2000, 2, 4, 8, 0x80 };

    gl_reset();
    gl_make_current(qts);
    set_hints(qts, hints_a);
    hint_push(qts, 0x8000);
    set_hints(qts, hints_b);
    hint_push(qts, 0x8000);
    set_hints(qts, hints_default);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(qts, hints_b);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(qts, hints_a);
    assert_attrib_depth(qts, 0);
    for (unsigned k = 0; k < ARRAY_SIZE(other_groups); k++) {
        set_hints(qts, hints_a);
        prepare_refused_reply(qts);
        kona_command(qts, 0xc9, &other_groups[k], 1);
        set_hints(qts, hints_b);
        kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
        assert_hints(qts, hints_b);
    }
    set_hints(qts, hints_a);
    kona_command_f(qts, 0x168, (float[]){ 2, 3 }, 2);
    hint_push(qts, 0x8008);
    set_hints(qts, hints_b);
    kona_command_f(qts, 0x168, (float[]){ 5, 7 }, 2);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(qts, hints_a);
    kona_command(qts, 0x4c, (uint32_t[]){ 0x8038 }, 1);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 0x40000000);
    for (unsigned k = 0; k < 2; k++) {
        prepare_refused_reply(qts);
        kona_command(qts, 0xc9,
                     (uint32_t[]){ 0x8000 | (k ? 0x1000 : 0x40000) }, 1);
        assert_reply_unchanged(qts);
        assert_hints(qts, hints_a);
        assert_attrib_depth(qts, 0);
    }
    for (unsigned k = 0; k < 16; k++) {
        hint_push(qts, 0x8000);
    }
    set_hints(qts, hints_b);
    hint_push(qts, 0x8000);
    assert_attrib_depth(qts, 16);
    for (unsigned k = 0; k < 16; k++) {
        kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
        assert_hints(qts, hints_a);
    }
    set_hints(qts, hints_b);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(qts, hints_b);
    hint_push(qts, 0x8000);
    set_hints(qts, hints_a);
    kona_command(qts, 0x57, (uint32_t[]){ 0 }, 1);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    kona_command(qts, 0x71, (uint32_t[]){ 0 }, 1);
    assert_hints(qts, hints_a);
    assert_attrib_depth(qts, 1);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(qts, hints_b);
}

static void test_native_hints_contexts(void)
{
    const uint64_t save_b = SAVE_AREA + 0x20000;
    const uint32_t single[10] = { 0, 12 };
    const uint32_t twice[10] = { 0, 12, 0, 1 };

    gl_reset();
    gl_make_current(qts);
    set_hints(qts, hints_a);
    hint_push(qts, 0x8000);
    return_context_deactivate(qts);
    attrib_context_fresh(qts, save_b);
    assert_hints(qts, hints_default);
    set_hints(qts, hints_b);
    for (unsigned k = 0; k < 2; k++) {
        return_context_deactivate(qts);
        return_context_restore(qts, SAVE_AREA);
        assert_hints(qts, hints_a);
        return_context_deactivate(qts);
        return_context_restore(qts, save_b);
        assert_hints(qts, hints_b);
    }
    return_context_deactivate(qts);
    return_context_restore(qts, SAVE_AREA);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    assert_hints(qts, hints_a);
    place_read_window(qts, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
    assert_hints(qts, hints_a);
    kona_command(qts, 1, twice, 10);
    assert_hints(qts, hints_a);
    kona_command(qts, 1, single, 10);
    assert_hints(qts, hints_a);
    kona_command_kicked(qts, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE + 0x4000),
                                      tagged_offset_word(RETURN_PAGE + 0x4000),
                                      0x4000 }, 3);
    kona_command_kicked(qts, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    assert_hints(qts, hints_a);
    return_context_deactivate(qts);
    attrib_context_fresh(qts, SAVE_AREA);
    assert_hints(qts, hints_default);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(qts, hints_default);
    assert_attrib_depth(qts, 0);
}

static void test_native_hints_reset(void)
{
    for (unsigned words = 0; words < 3; words++) {
        gl_reset();
        gl_make_current(qts);
        set_hints(qts, hints_a);
        hint_push(qts, 0x8000);
        kona_command(qts, 0xbb, (uint32_t[]){ 0xc50 }, words ? 1 : 0);
        if (words == 2) {
            qtest_writel(qts, REG_COMMAND, 0x1102);
            assert_hint(qts, 0, 2);
            kona_command(qts, 0xc9, NULL, 0);
        }
        gl_reset();
        gl_make_current(qts);
        assert_hints(qts, hints_default);
        assert_attrib_depth(qts, 0);
        set_hints(qts, hints_b);
        assert_hints(qts, hints_b);
    }
    gl_reset();
    gl_make_current(qts);
    set_hints(qts, hints_a);
    hint_push(qts, 0x8000);
    set_hints(qts, hints_b);
    kona_command(qts, 0xc8, NULL, 0);
    gl_reset();
    gl_make_current(qts);
    assert_hints(qts, hints_default);
    assert_attrib_depth(qts, 0);
}

static void hints_after_migration(QTestState *s)
{
    assert_hints(s, hints_default);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(s, hints_b);
    return_context_deactivate(s);
    return_context_restore(s, SAVE_AREA);
    assert_hints(s, hints_default);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(s, hints_b);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_hints(s, hints_a);
}

static void test_native_hints_migration(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-kona-hints-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_init(args);
    gl_make_current(from);
    if (kind == 0) {
        set_hints(from, hints_a);
        hint_push(from, 0x8000);
        set_hints(from, hints_b);
        hint_push(from, 0x8000);
        set_hints(from, hints_default);
        return_context_deactivate(from);
        attrib_context_fresh(from, SAVE_AREA + 0x20000);
        set_hints(from, hints_b);
        hint_push(from, 0x8000);
        set_hints(from, hints_default);
    } else if (kind < 3) {
        set_hints(from, hints_a);
        prepare_refused_reply(from);
        kona_command(from, 0xbb, (uint32_t[]){ 0xc54 }, kind - 1);
        assert_reply_unchanged(from);
    } else {
        gl_make_current(to);
        set_hints(to, hints_b);
    }
    sgi_migrate_stopped(from, uri);
    if (kind == 0) {
        hints_after_migration(to);
        hints_after_migration(from);
    } else if (kind < 3) {
        const uint8_t expected[5] = { 1, 2, 0, 1, 1 };
        QTestState *states[] = { to, from };

        for (unsigned k = 0; k < ARRAY_SIZE(states); k++) {
            if (kind == 1) {
                qtest_writel(states[k], REG_COMMAND, 0xc54);
            }
            qtest_writel(states[k], REG_COMMAND, 0x1101);
            assert_reply_unchanged(states[k]);
            assert_hints(states[k], expected);
        }
    } else {
        assert_hints(to, hints_default);
        assert_hints(from, hints_default);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_native_hints_refused(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);

    if (kind == 0) {
        gl_make_current(s);
        return_context_deactivate(s);
    } else {
        kona_command_kicked(s, 0x1fd,
                            (uint32_t[]){ physical_page_word(SAVE_AREA),
                                          tagged_offset_word(SAVE_AREA),
                                          0x10000 }, 3);
        if (kind == 2) {
            kona_command_kicked(s, 0x1f2,
                                (uint32_t[]){ physical_page_word(0x08000000),
                                              tagged_offset_word(0x08000000),
                                              0x4000 }, 3);
        }
    }
    for (unsigned k = 0; k < 5; k++) {
        prepare_refused_reply(s);
        kona_command(s, 0x4c, (uint32_t[]){ 0xc50 + k }, 1);
        assert_reply_unchanged(s);
    }
    qtest_quit(s);
}

typedef struct NativeQueryState {
    uint32_t stencil[9];
    uint32_t clips[6];
    uint32_t lights[8];
} NativeQueryState;

static const NativeQueryState native_query_default = {
    .stencil = { 0, 0, 0x207, UINT32_MAX, 0x1e00, 0x1e00, 0x1e00,
                 0, UINT32_MAX },
};
static const NativeQueryState native_query_a = {
    .stencil = { 1, 0x35, 0x202, 0xa55a00f3, 0x1e01, 0x1e02, 0x1e03,
                 0x17, 0x5aa500cf },
    .clips = { 1, 0, 1, 0, 0, 1 },
    .lights = { 1, 0, 0, 1, 0, 0, 0, 1 },
};
static const NativeQueryState native_query_b = {
    .stencil = { 0, 0x6a, 0x203, 0x135700aa, 0x1e03, 0x1e01, 0x1e02,
                 0x2b, 0x24680055 },
    .clips = { 0, 0, 0, 0, 0, 1 },
    .lights = { 0, 0, 0, 0, 0, 0, 0, 1 },
};

/* Only one signed32 payload word may replace the patterned return page. */
static void assert_native_query_word(QTestState *s, uint32_t pname,
                                     uint32_t value, bool wide)
{
    uint8_t got[0x4000 - 4], expected[0x4000 - 4];

    memset(expected, 0xa5, sizeof(expected));
    expected[4] = value >> 24;
    expected[5] = value >> 16;
    expected[6] = value >> 8;
    expected[7] = value;
    prepare_refused_reply(s);
    if (wide) {
        qtest_writeq(s, REG_COMMAND, (UINT64_C(0x4c) << 32) | pname);
    } else {
        kona_command(s, 0x4c, &pname, 1);
    }
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 4, got, sizeof(got));
    if (memcmp(got, expected, sizeof(got))) {
        g_test_message("pname 0x%x: expected 0x%08x, received 0x%08x",
                       pname, value, ldl_be_p(got + 4));
    }
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
}

static void assert_native_query_state(QTestState *s, const NativeQueryState *v,
                                      bool wide)
{
    for (unsigned k = 0; k < ARRAY_SIZE(v->stencil); k++) {
        assert_native_query_word(s, 0xb90 + k, v->stencil[k], wide);
    }
    for (unsigned k = 0; k < ARRAY_SIZE(v->clips); k++) {
        assert_native_query_word(s, 0x3000 + k, v->clips[k], wide);
    }
    for (unsigned k = 0; k < ARRAY_SIZE(v->lights); k++) {
        assert_native_query_word(s, 0x4000 + k, v->lights[k], wide);
    }
}

static void set_native_query_state(QTestState *s, const NativeQueryState *v)
{
    kona_command(s, v->stencil[0] ? 0x24 : 0x25,
                 (uint32_t[]){ 0xb90 }, 1);
    kona_command(s, 0x1e, &v->stencil[1], 1);
    kona_command(s, 0xcc,
                 (uint32_t[]){ v->stencil[2], v->stencil[7], v->stencil[3] },
                 3);
    kona_command(s, 0xcd, v->stencil + 4, 3);
    kona_command(s, 0x20, &v->stencil[8], 1);
    for (unsigned k = 0; k < ARRAY_SIZE(v->clips); k++) {
        kona_command(s, v->clips[k] ? 0x24 : 0x25,
                     (uint32_t[]){ 0x3000 + k }, 1);
    }
    for (unsigned k = 0; k < ARRAY_SIZE(v->lights); k++) {
        kona_command(s, v->lights[k] ? 0x24 : 0x25,
                     (uint32_t[]){ 0x4000 + k }, 1);
    }
}

static void test_native_query_bytes(void)
{
    gl_reset();
    gl_make_current(qts);
    assert_native_query_state(qts, &native_query_default, false);
    set_native_query_state(qts, &native_query_a);
    assert_native_query_state(qts, &native_query_a, false);
    assert_native_query_state(qts, &native_query_a, true);
    set_native_query_state(qts, &native_query_b);
    assert_native_query_state(qts, &native_query_b, false);
    qtest_writeq(qts, REG_COMMAND, (UINT64_C(0xcc) << 32) | 0x202);
    qtest_writeq(qts, REG_COMMAND, (UINT64_C(0x17) << 32) | 0xa55a00f3);
    assert_native_query_word(qts, 0xb92, 0x202, false);
    assert_native_query_word(qts, 0xb93, 0xa55a00f3, false);
    assert_native_query_word(qts, 0xb97, 0x17, false);
}

static void test_native_indexed_query_bits(void)
{
    NativeQueryState expected = native_query_a;

    gl_reset();
    gl_make_current(qts);
    set_native_query_state(qts, &expected);
    for (unsigned k = 0; k < ARRAY_SIZE(expected.clips); k++) {
        if (expected.clips[k]) {
            qtest_writeq(qts, REG_COMMAND,
                         (UINT64_C(0x25) << 32) | (0x3000 + k));
            expected.clips[k] = 0;
            assert_native_query_state(qts, &expected, false);
        }
    }
    for (unsigned k = 0; k < ARRAY_SIZE(expected.lights); k++) {
        if (expected.lights[k]) {
            qtest_writeq(qts, REG_COMMAND,
                         (UINT64_C(0x25) << 32) | (0x4000 + k));
            expected.lights[k] = 0;
            assert_native_query_state(qts, &expected, false);
        }
    }
    for (unsigned k = 0; k < ARRAY_SIZE(expected.clips); k++) {
        qtest_writeq(qts, REG_COMMAND,
                     (UINT64_C(0x24) << 32) | (0x3000 + k));
        expected.clips[k] = 1;
        assert_native_query_state(qts, &expected, false);
    }
    for (unsigned k = 0; k < ARRAY_SIZE(expected.lights); k++) {
        qtest_writeq(qts, REG_COMMAND,
                     (UINT64_C(0x24) << 32) | (0x4000 + k));
        expected.lights[k] = 1;
        assert_native_query_state(qts, &expected, false);
    }
}

static void test_native_query_invalid(void)
{
    const uint32_t unknown[] = { 0x2fff, 0x3006, 0x3fff, 0x4008, 0xd33 };

    gl_reset();
    gl_make_current(qts);
    set_native_query_state(qts, &native_query_a);
    kona_command(qts, 0xcc, (uint32_t[]){ 0x208, 0x2b, 0x12345678 }, 3);
    assert_native_query_state(qts, &native_query_a, false);
    kona_command(qts, 0xcd, (uint32_t[]){ 0x1e03, 0xffffffff, 0x1e01 }, 3);
    assert_native_query_state(qts, &native_query_a, false);
    for (unsigned k = 0; k < ARRAY_SIZE(unknown); k++) {
        kona_command(qts, 0x24, &unknown[k], 1);
        assert_native_query_state(qts, &native_query_a, false);
        prepare_refused_reply(qts);
        kona_command(qts, 0x4c, &unknown[k], 1);
        assert_reply_unchanged(qts);
    }
}

static void test_native_query_attrib(void)
{
    const uint32_t groups[] = { 0x400, 0x2000, 0x40 };

    gl_reset();
    gl_make_current(qts);
    for (unsigned k = 0; k < ARRAY_SIZE(groups); k++) {
        NativeQueryState expected = native_query_b;

        set_native_query_state(qts, &native_query_a);
        prepare_refused_reply(qts);
        kona_command(qts, 0xc9, &groups[k], 1);
        assert_reply_posted(qts);
        set_native_query_state(qts, &native_query_b);
        kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
        if (groups[k] == 0x400) {
            memcpy(expected.stencil, native_query_a.stencil,
                   sizeof(expected.stencil));
        } else {
            memcpy(expected.lights, native_query_a.lights,
                   sizeof(expected.lights));
            if (groups[k] == 0x2000) {
                memcpy(expected.clips, native_query_a.clips,
                       sizeof(expected.clips));
                expected.stencil[0] = native_query_a.stencil[0];
            }
        }
        assert_native_query_state(qts, &expected, false);
        assert_attrib_depth(qts, 0);
    }
    set_native_query_state(qts, &native_query_a);
    kona_command(qts, 0xc9, (uint32_t[]){ 0x2400 }, 1);
    set_native_query_state(qts, &native_query_b);
    prepare_refused_reply(qts);
    kona_command(qts, 0xc9, (uint32_t[]){ 0x3400 }, 1);
    assert_reply_unchanged(qts);
    assert_attrib_depth(qts, 1);
    assert_native_query_state(qts, &native_query_b, false);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_query_state(qts, &native_query_a, false);
}

static void test_native_stencil_query_pixels(void)
{
    NativeQueryState expected = native_query_default;
    const uint32_t stencil[] = {
        1, 53, 0x202, 255, 0x1e00, 0x1e00, 0x1e00, 53, 255,
    };

    gl_reset();
    stencil_setup(qts);
    stencil_clear(qts, 53, 255);
    stencil_function(qts, 0x202, 53, 255);
    stencil_operations(qts, 0x1e00, 0x1e00, 0x1e00);
    memcpy(expected.stencil, stencil, sizeof(stencil));
    assert_native_query_state(qts, &expected, false);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0xff0000);
    kona_command(qts, 0xc9, (uint32_t[]){ 0x400 }, 1);
    kona_command(qts, 0xcc, (uint32_t[]){ 0x200, 0, 1 }, 3);
    kona_command(qts, 0xcd, (uint32_t[]){ 0x1e02, 0x1e03, 0x1e01 }, 3);
    kona_command(qts, 0x20, (uint32_t[]){ 3 }, 1);
    kona_command(qts, 0x1e, (uint32_t[]){ 66 }, 1);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_query_state(qts, &expected, false);
    kona_command_f(qts, 0x1d, (float[]){ 0, 0, 1, 1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    stencil_triangle(qts);
    assert_stencil_colour(qts, 0xff0000);
}

static void test_native_query_contexts(void)
{
    const uint64_t save_b = SAVE_AREA + 0x20000;

    gl_reset();
    gl_make_current(qts);
    set_native_query_state(qts, &native_query_a);
    kona_command(qts, 0xc9, (uint32_t[]){ 0x2400 }, 1);
    return_context_deactivate(qts);
    attrib_context_fresh(qts, save_b);
    assert_native_query_state(qts, &native_query_default, false);
    set_native_query_state(qts, &native_query_b);
    for (unsigned k = 0; k < 2; k++) {
        return_context_deactivate(qts);
        return_context_restore(qts, SAVE_AREA);
        assert_native_query_state(qts, &native_query_a, false);
        return_context_deactivate(qts);
        return_context_restore(qts, save_b);
        assert_native_query_state(qts, &native_query_b, false);
    }
    return_context_deactivate(qts);
    return_context_restore(qts, SAVE_AREA);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    assert_native_query_state(qts, &native_query_a, false);
    place_read_window(qts, PROBE_X, PROBE_Y, PROBE_SIZE, PROBE_SIZE);
    assert_native_query_state(qts, &native_query_a, false);
    return_context_deactivate(qts);
    attrib_context_fresh(qts, SAVE_AREA);
    assert_native_query_state(qts, &native_query_default, false);
    kona_command(qts, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_query_state(qts, &native_query_default, false);
    assert_attrib_depth(qts, 0);
}

static void test_native_query_reset(void)
{
    for (unsigned kind = 0; kind < 3; kind++) {
        gl_reset();
        gl_make_current(qts);
        set_native_query_state(qts, &native_query_a);
        kona_command(qts, 0xc9, (uint32_t[]){ 0x2400 }, 1);
        if (kind == 0) {
            kona_command(qts, 0xcc, (uint32_t[]){ 0x203, 0x2b }, 2);
        } else if (kind == 1) {
            kona_command(qts, 0xcd, (uint32_t[]){ 0x1e03, 0x1e01 }, 2);
        } else {
            kona_command(qts, 0x24, NULL, 0);
        }
        gl_reset();
        gl_make_current(qts);
        assert_native_query_state(qts, &native_query_default, false);
        assert_attrib_depth(qts, 0);
        set_native_query_state(qts, &native_query_b);
        assert_native_query_state(qts, &native_query_b, false);
    }
}

static void native_queries_pop_after_migration(QTestState *s)
{
    assert_native_query_state(s, &native_query_default, false);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_query_state(s, &native_query_b, false);
    return_context_deactivate(s);
    return_context_restore(s, SAVE_AREA);
    assert_native_query_state(s, &native_query_default, false);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_query_state(s, &native_query_b, false);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_query_state(s, &native_query_a, false);
}

static void test_native_query_migration(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    NativeQueryState expected = native_query_a;
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-kona-native-queries-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_init(args);
    gl_make_current(from);
    if (kind == 0) {
        set_native_query_state(from, &native_query_a);
        kona_command(from, 0xc9, (uint32_t[]){ 0x2400 }, 1);
        set_native_query_state(from, &native_query_b);
        kona_command(from, 0xc9, (uint32_t[]){ 0x2400 }, 1);
        set_native_query_state(from, &native_query_default);
        return_context_deactivate(from);
        attrib_context_fresh(from, SAVE_AREA + 0x20000);
        set_native_query_state(from, &native_query_b);
        kona_command(from, 0xc9, (uint32_t[]){ 0x2400 }, 1);
        set_native_query_state(from, &native_query_default);
    } else if (kind < 4) {
        set_native_query_state(from, &native_query_a);
        prepare_refused_reply(from);
        if (kind == 1) {
            kona_command(from, 0xcc, (uint32_t[]){ 0x203, 0x19 }, 2);
            expected.stencil[2] = 0x203;
            expected.stencil[3] = 0x12345678;
            expected.stencil[7] = 0x19;
        } else if (kind == 2) {
            kona_command(from, 0xcd, (uint32_t[]){ 0x1e03, 0x1e01 }, 2);
            expected.stencil[4] = 0x1e03;
            expected.stencil[5] = 0x1e01;
            expected.stencil[6] = 0x1e02;
        } else {
            kona_command(from, 0x24, NULL, 0);
            expected.clips[1] = 1;
        }
        assert_reply_unchanged(from);
    }
    sgi_migrate_stopped(from, uri);
    for (unsigned k = 0; k < 2; k++) {
        QTestState *s = k ? from : to;

        if (kind == 0) {
            native_queries_pop_after_migration(s);
        } else if (kind < 4) {
            qtest_writel(s, REG_COMMAND,
                         kind == 1 ? 0x12345678 : kind == 2 ? 0x1e02 : 0x3001);
            assert_reply_unchanged(s);
            assert_native_query_state(s, &expected, false);
        } else {
            assert_native_query_state(s, &native_query_default, false);
        }
    }
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_native_query_refused(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);
    const uint32_t first[] = { 0xb90, 0x3000, 0x4000 };
    const unsigned count[] = { 9, 6, 8 };

    if (kind == 0) {
        gl_make_current(s);
        return_context_deactivate(s);
    } else {
        kona_command_kicked(s, 0x1fd,
                            (uint32_t[]){ physical_page_word(SAVE_AREA),
                                          tagged_offset_word(SAVE_AREA),
                                          0x10000 }, 3);
        if (kind == 2) {
            kona_command_kicked(s, 0x1f2,
                                (uint32_t[]){ physical_page_word(0x08000000),
                                              tagged_offset_word(0x08000000),
                                              0x4000 }, 3);
        }
    }
    for (unsigned g = 0; g < ARRAY_SIZE(first); g++) {
        for (unsigned k = 0; k < count[g]; k++) {
            prepare_refused_reply(s);
            kona_command(s, 0x4c, (uint32_t[]){ first[g] + k }, 1);
            assert_reply_unchanged(s);
        }
    }
    qtest_quit(s);
}

/* Literal first native header and source bytes from the PIO capture. */
static const uint32_t texture_header[20] = {
    0xde1, 2, 2, 5, 1, 0, 0, 0, 0, 0x30,
    0, 0, 0, 1, 1, 0, 0x23, 0x2181, 0, 0,
};
static const uint32_t texture_source[8] = {
    0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    0x00ffffff, 0xff00ffff, 0xffff00ff, 0x000000ff,
};

static void assert_texture_staging(QTestState *s, const uint32_t *v,
                                   unsigned n, unsigned have)
{
    QDict *reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gl-data-have'}}");

    g_assert_cmpuint(qdict_get_int(reply, "return"), ==, have);
    qobject_unref(reply);
    for (unsigned k = 0; k < n; k++) {
        qtest_qmp_assert_success(s,
            "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
            "'property':'x-test-gc-index','value':%u}}", k);
        reply = qtest_qmp(s,
            "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
            "'property':'x-test-gl-data-word'}}");
        g_assert_cmphex(qdict_get_int(reply, "return"), ==, v[k]);
        qobject_unref(reply);
    }
}

static void texture_native_bind(QTestState *s, uint32_t handle)
{
    uint32_t record[22] = { 0, 0x101010, 256u << 10 };

    kona_command(s, 0x1d5, (uint32_t[]) { handle, 0, 0x30 << 14, 0x11, 0 }, 5);
    kona_command(s, 0x179, record, 22);
}

static void assert_texture_pixel(QTestState *s, uint32_t x, uint32_t y,
                                   uint32_t expected)
{
    QDict *reply;

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-x','value':%u}}", x);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-y','value':%u}}", y);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-texture-pixel'}}");
    g_assert_cmphex(qdict_get_int(reply, "return"), ==, expected);
    qobject_unref(reply);
}

/* Incomplete rows and mismatched publication never replace a sampled image. */
static void test_texture_native_publication(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t h[20];

    gl_make_current(s);
    memcpy(h, texture_header, sizeof(h));
    h[10] = 0x12345678;
    texture_native_bind(s, h[10]);
    kona_command(s, 0x174, h, 20);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, texture_source[k]);
    }
    assert_texture_pixel(s, 0, 0, 0);
    kona_command(s, 0x176, (uint32_t[]) { h[10] }, 1);
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2, texture_source[k]);
    }
    kona_command(s, 0x174, h, 20);
    for (unsigned k = 0; k < 3; k++) {
        qtest_writel(s, REG_COMMAND, 0xaabbccdd);
    }
    assert_texture_pixel(s, 0, 0, texture_source[0]);
    qtest_writel(s, REG_COMMAND, 0xaabbccdd);
    kona_command(s, 0x176, (uint32_t[]) { h[10] + 1 }, 1);
    assert_texture_pixel(s, 0, 0, texture_source[0]);
    kona_command(s, 0x1c5, (uint32_t[]) { h[10] }, 1);
    assert_texture_pixel(s, 0, 0, 0);
    /* A table supplied after upload resolves the same stored source bytes. */
    kona_command(s, 0x174, h, 20);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, texture_source[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { h[10] }, 1);
    texture_native_bind(s, h[10]);
    assert_texture_pixel(s, 1, 1, texture_source[3]);
    kona_command(s, 0x174, h, 20);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, 0xaabbccdd);
    }
    kona_command(s, 0x1c5, (uint32_t[]) { h[10] }, 1);
    kona_command(s, 0x176, (uint32_t[]) { h[10] }, 1);
    texture_native_bind(s, h[10]);
    assert_texture_pixel(s, 0, 0, 0);
    kona_command(s, 0x174, h, 20);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, 0xaabbccdd);
    }
    kona_command(s, 0xb, NULL, 0);
    for (unsigned k = 0; k < 32; k++) {
        qtest_writel(s, REG_COMMAND, 0);
    }
    kona_command(s, 0x176, (uint32_t[]) { h[10] }, 1);
    assert_texture_pixel(s, 0, 0, 0);
    qtest_system_reset(s);
    assert_texture_pixel(s, 0, 0, 0);
    qtest_quit(s);
}

static void test_texture_native_draw(void)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    texture_native_bind(s, 0);
    kona_command(s, 0x174, texture_header, 20);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, texture_source[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0xff0000);
    kona_command(s, 0xed, (uint32_t[]) { 0, 10, 0x80ffffff, 0x00008000 }, 4);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0x800080);
    kona_command(s, 0x1d7, (uint32_t[]) { 0, 0, 0, 0 }, 4);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0xff0000);
    qtest_quit(s);
}

static void texture_dma_packet(QTestState *s, uint64_t io, uint32_t total,
                                uint32_t per_line, uint32_t gap);

static void texture_float_rows(QTestState *s, const uint32_t *pixels,
                                unsigned prefix, unsigned first, unsigned end)
{
    unsigned row_words = DIV_ROUND_UP(prefix + 32, 4);

    for (unsigned row = 0; row < 2; row++) {
        for (unsigned word = 0; word < DIV_ROUND_UP(prefix + 32, 4); word++) {
            uint32_t value = 0;

            if (row * row_words + word < first ||
                row * row_words + word >= end) {
                continue;
            }
            for (unsigned k = 0; k < 4; k++) {
                int offset = word * 4 + k - prefix;
                uint32_t byte = 0xee;

                if (offset >= 0 && offset < 32) {
                    byte = (pixels[row * 8 + offset / 4] >>
                            (24 - (offset % 4) * 8)) & 255;
                }
                value = (value << 8) | byte;
            }
            qtest_writel(s, REG_COMMAND, value);
        }
    }
}

static void test_texture_native_float(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode == 1;
    uint32_t source[] = {
        0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000,
        0, 0x3f800000, 0, 0x3f800000,
        0, 0, 0x3f800000, 0x3f800000,
        0x3f800000, 0, 0, 0x3f800000,
    };
    uint32_t h[20];
    QTestState *s = qtest_init(MACHINE_ARGS);
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    texture_native_bind(s, 0);
    memcpy(h, texture_header, sizeof(h));
    h[4] = 6;
    h[5] = 3;
    h[6] = dma;
    h[7] = dma;
    h[17] |= dma ? 4 : 0;
    kona_command(s, 0x174, h, 20);
    if (dma) {
        uint8_t bytes[sizeof(source)];

        for (unsigned k = 0; k < ARRAY_SIZE(source); k++) {
            stl_be_p(bytes + k * 4, source[k]);
        }
        qtest_memwrite(s, 0x380003, bytes, sizeof(bytes));
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        texture_dma_packet(s, 3, sizeof(bytes), 32, 0);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else {
        texture_float_rows(s, source, 3, 0, 18);
    }
    assert_texture_pixel(s, 0, 0, 0);
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(s, 0, 0, 0x4080bfff);
    assert_texture_pixel(s, 1, 1, 0xff0000ff);
    if (mode == 2) {
        /* Refusing a nonfinite last component preserves the published image. */
        source[15] = 0x7fc00000;
        kona_command(s, 0x174, h, 20);
        texture_float_rows(s, source, 3, 0, 18);
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
        assert_texture_pixel(s, 0, 0, 0x4080bfff);
        assert_texture_pixel(s, 1, 1, 0xff0000ff);
    }
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0x4080bf);
    qtest_quit(s);
}

static void test_texture_float_migration(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode >= 2;
    uint32_t source[] = {
        0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000,
        0, 0x3f800000, 0, 0x3f800000,
        0, 0, 0x3f800000, 0x3f800000,
        0x3f800000, 0, 0, 0x3f800000,
    };
    uint32_t h[20];
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to;

    gl_make_current(from);
    texture_native_bind(from, 0);
    memcpy(h, texture_header, sizeof(h));
    h[4] = 6;
    h[5] = 3;
    h[6] = h[7] = dma;
    h[17] |= dma ? 4 : 0;
    if (dma) {
        uint8_t bytes[sizeof(source)];

        for (unsigned k = 0; k < ARRAY_SIZE(source); k++) {
            stl_be_p(bytes + k * 4, source[k]);
        }
        qtest_memwrite(from, 0x380003, bytes, sizeof(bytes));
        qtest_writel(from, KONA_REG(0x100000), 0x20000380);
    }
    kona_command(from, 0x174, h, 20);
    if (!dma) {
        texture_float_rows(from, source, 3, 0, mode == 0 ? 7 : 18);
    } else if (mode == 3) {
        texture_dma_packet(from, 3, sizeof(source), 32, 0);
    }
    dir = g_dir_make_tmp("sn0-kona-float-XXXXXX", &error);
    g_assert_no_error(error);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    assert_texture_pixel(to, 0, 0, 0);
    if (mode == 0) {
        texture_float_rows(to, source, 3, 7, 18);
    } else if (mode == 2) {
        texture_dma_packet(to, 3, sizeof(source), 32, 0);
        g_assert_cmphex(qtest_readl(to, REG_MAILBOX), ==, 0x100);
    }
    kona_command(to, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(to, 0, 0, 0x4080bfff);
    assert_texture_pixel(to, 1, 1, 0xff0000ff);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_texture_native_byte_formats(gconstpointer opaque)
{
    static const struct {
        uint32_t source, storage, bytes, pixel, first, last, rgb;
    } cases[] = {
        { 3, 0xe, 1, 0x80000000, 0x00000080, 0x00000083, 0x8080ff },
        { 4, 0xd, 3, 0x4090e000, 0x400000ff, 0x430000ff, 0x404040 },
        { 6, 0xd, 1, 0x40000000, 0x400000ff, 0x430000ff, 0x404040 },
        { 7, 8, 2, 0x40800000, 0x40000080, 0x43000083, 0x20209f },
    };
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    unsigned c = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t h[20];
    uint32_t record[22] = { 0, 0x101010, 256u << 10 };

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    memcpy(h, texture_header, sizeof(h));
    h[3] = cases[c].source;
    h[9] = cases[c].storage;
    h[17] = 0x2180;
    kona_command(s, 0x1d5,
                 (uint32_t[]) { 0, 0, h[9] << 14, 0x11, 0 }, 5);
    kona_command(s, 0x179, record, 22);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
    for (unsigned alignment = 1; alignment <= 8; alignment *= 2) {
        uint32_t stride = ROUND_UP(3 * cases[c].bytes, alignment);

        kona_command(s, 0x33, (uint32_t[]) { 0xcf5, alignment }, 2);
        for (unsigned pad = 0; pad < 4; pad++) {
            h[5] = pad;
            kona_command(s, 0x174, h, 20);
            for (unsigned y = 0; y < 2; y++) {
                unsigned phase = (pad + y * stride) & 3;
                unsigned words = DIV_ROUND_UP(phase + 2 * cases[c].bytes, 4);

                for (unsigned x = 0; x < words; x++) {
                    uint32_t word = 0;

                    for (unsigned k = 0; k < 4; k++) {
                        int offset = x * 4 + k - phase;
                        unsigned byte = 0xee;

                        if (offset >= 0 && offset < 2 * cases[c].bytes) {
                            unsigned component = offset % cases[c].bytes;
                            unsigned pixel = y * 2 + offset / cases[c].bytes;

                            byte = ((cases[c].pixel >>
                                     (24 - component * 8)) & 255) + pixel;
                        }
                        word = (word << 8) | byte;
                    }
                    qtest_writel(s, REG_COMMAND, word);
                }
            }
            kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
            assert_texture_pixel(s, 0, 0, cases[c].first);
            assert_texture_pixel(s, 1, 1, cases[c].last);
        }
    }
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command(s, 0x24, (uint32_t[]) { 0xbe2 }, 1);
    kona_command(s, 0xcb, (uint32_t[]) { 0x302, 0x303 }, 2);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, cases[c].rgb);
    qtest_quit(s);
}

static void test_texture_native_rows(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t h[20];
    uint32_t record[22] = { 0, 0x101010, 256u << 10, 0, 0, 0, 0x55 };

    gl_make_current(s);
    memcpy(h, texture_header, sizeof(h));
    h[1] = h[2] = 16;
    h[13] = h[14] = 15;
    h[16] = 0x44;
    h[18] = 0x55;
    kona_command(s, 0x1d5, (uint32_t[]) { 0, 0, 0x30 << 14, 0x44, 0 }, 5);
    /* Select the physical descriptor supplied by this upload. */
    kona_command(s, 0x179, record, 22);
    for (unsigned pad = 0; pad < 4; pad++) {
        h[5] = pad;
        kona_command(s, 0x174, h, 20);
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16 + !!pad; x++) {
                uint32_t word = 0;

                for (unsigned k = 0; k < 4; k++) {
                    int offset = x * 4 + k - pad;
                    unsigned byte = 0xee;

                    if (offset >= 0 && offset < 64) {
                        uint32_t pixel = 0x102030ff +
                                         ((offset / 4) << 24) + (y << 16);

                        byte = (pixel >> (24 - (offset % 4) * 8)) & 0xff;
                    }
                    word = (word << 8) | byte;
                }
                qtest_writel(s, REG_COMMAND, word);
            }
        }
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
        assert_texture_pixel(s, 0, 0, 0x102030ff);
        assert_texture_pixel(s, 15, 15, 0x1f2f30ff);
    }
    qtest_quit(s);
}

static void test_texture_native_migrates(gconstpointer opaque)
{
    unsigned before = GPOINTER_TO_UINT(opaque);
    g_autofree char *tmpfs = NULL, *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-texture-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    gl_make_current(from);
    texture_native_bind(from, 0);
    kona_command(from, 0x174, texture_header, 20);
    for (unsigned k = 0; k < before; k++) {
        qtest_writel(from, REG_COMMAND, texture_source[k]);
    }
    sgi_migrate_stopped(from, uri);
    assert_texture_pixel(to, 0, 0, 0);
    for (unsigned k = before; k < 4; k++) {
        qtest_writel(to, REG_COMMAND, texture_source[k]);
    }
    kona_command(to, 0x176, (uint32_t[]) { 0 }, 1);
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(to, k % 2, k / 2, texture_source[k]);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_texture_byte_migrates(gconstpointer opaque)
{
    static const uint32_t words[] = {
        0xeeeeee40, 0x90e04191, 0xe1eeeeee, 0x4292e243, 0x93e3eeee,
    };
    unsigned before = GPOINTER_TO_UINT(opaque);
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint32_t h[20];
    uint32_t record[22] = { 0, 0x101010, 256u << 10 };

    dir = g_dir_make_tmp("sn0-kona-texture-byte-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    gl_make_current(from);
    memcpy(h, texture_header, sizeof(h));
    h[3] = 4;
    h[5] = 3;
    h[9] = 0xd;
    h[17] = 0x2180;
    kona_command(from, 0x1d5,
                 (uint32_t[]) { 0, 0, 0xd << 14, 0x11, 0 }, 5);
    kona_command(from, 0x179, record, 22);
    kona_command(from, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
    kona_command(from, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(from, 0x174, h, 20);
    for (unsigned k = 0; k < before; k++) {
        qtest_writel(from, REG_COMMAND, words[k]);
    }
    sgi_migrate_stopped(from, uri);
    assert_texture_pixel(to, 0, 0, 0);
    for (unsigned k = before; k < ARRAY_SIZE(words); k++) {
        qtest_writel(to, REG_COMMAND, words[k]);
    }
    kona_command(to, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(to, 0, 0, 0x400000ff);
    assert_texture_pixel(to, 1, 1, 0x430000ff);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void texture_dma_packet(QTestState *s, uint64_t io, uint32_t total,
                                uint32_t per_line, uint32_t gap)
{
    uint32_t packet[] = {
        0x1eb, io >> 12, (io & 0xfff) | 0x9000,
        total, per_line, gap, 0x100,
    };

    for (unsigned k = 0; k < ARRAY_SIZE(packet); k++) {
        qtest_writel(s, KONA_REG(0x80007c), packet[k]);
    }
}

static void test_texture_native_dma(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool contiguous = mode == 0 || mode == 10;
    bool fault = mode == 2;
    bool large = mode == 6 || mode == 11;
    bool migration = mode == 7 || mode == 8;
    bool reset = mode == 9;
    bool trace = mode == 12;
    uint32_t width = mode == 11 ? 4096 : large ? 2048 : 2;
    uint32_t row_bytes = width * 3;
    uint32_t stride = mode == 10 ? 3 : contiguous ? 8 :
                      large ? row_bytes : 9;
    uint32_t span = stride + row_bytes;
    uint64_t host = UINT64_C(0x380000);
    uint64_t io = fault ? 0x3ffd : 3;
    g_autofree uint8_t *source = g_malloc0(span);
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;
    g_autofree char *log = NULL;
    QTestState *from = trace ? kona_init_logged(&log) :
                              qtest_init(MACHINE_ARGS);
    QTestState *to = NULL, *s = from;
    uint32_t h[20];
    uint32_t record[22] = { 0, 0x101010, 256u << 10 };
    uint32_t total = contiguous ? span : row_bytes * 2;
    uint32_t per_line = contiguous ? span : row_bytes;
    bool refused = fault || (mode >= 3 && mode <= 5) || reset || mode == 10;

    gl_make_current(s);
    memcpy(h, texture_header, sizeof(h));
    h[1] = width;
    h[3] = 4;
    h[5] = io & 3;
    h[6] = 1;
    h[7] = !contiguous;
    h[9] = 0xd;
    h[13] = width - 1;
    h[16] = mode == 11 ? 0x1c : large ? 0x1b : 0x11;
    h[17] = 0x2184;
    kona_command(s, 0x1d5,
                 (uint32_t[]) { 0, 0, 0xd << 14, h[16], 0 }, 5);
    kona_command(s, 0x179, record, 22);
    kona_command(s, 0x33,
                 (uint32_t[]) { 0xcf2, mode == 10 ? 1 :
                                contiguous ? 2 : large ? width : 3 }, 2);
    kona_command(s, 0x33,
                 (uint32_t[]) { 0xcf5,
                                mode == 10 ? 1 : contiguous ? 4 : 1 }, 2);
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < width; x++) {
            source[y * stride + x * 3] = 0x40 + ((y * width + x) & 63);
            source[y * stride + x * 3 + 1] = 0x90;
            source[y * stride + x * 3 + 2] = 0xe0;
        }
    }
    qtest_memwrite(s, host + io, source, span);
    for (unsigned k = 0; k < DIV_ROUND_UP(io + span, 0x4000); k++) {
        if (!fault || !k) {
            qtest_writel(s, KONA_REG(0x100000 + k * 4),
                         0x20000000 | ((host + k * 0x4000) >> 12));
        }
    }
    kona_command(s, 0x174, h, 20);
    if (mode == 3) {
        total = per_line;
    } else if (mode == 4) {
        total += per_line;
    } else if (mode == 5) {
        io++;
    }
    if (reset) {
        qtest_system_reset(s);
    }
    if (mode != 7) {
        texture_dma_packet(s, io, total, per_line,
                           contiguous ? 0 : stride - row_bytes);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, refused ? 0 : 0x100);
        assert_texture_pixel(s, 0, 0, 0);
    }
    if (migration) {
        dir = g_dir_make_tmp("sn0-kona-texture-dma-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
        if (mode == 7) {
            texture_dma_packet(s, io, total, per_line, stride - row_bytes);
            g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
        }
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(s, 0, 0, refused ? 0 : 0x400000ff);
    assert_texture_pixel(s, width - 1, 1,
                          refused ? 0 : (large ? 0x7f0000ff : 0x430000ff));
    if (to) {
        qtest_quit(to);
        g_rmdir(dir);
    }
    qtest_quit(from);
    if (trace) {
        g_autofree char *contents = kona_log_read(log);
        g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
        const char *payload[] = { "4090e04190e0", "4290e04390e0" };
        uint64_t kick = 0;
        unsigned count = 0;

        for (unsigned i = 0; lines[i]; i++) {
            const char *event = strstr(lines[i], "sgi_kona_pio_write seq ");
            uint64_t sequence, address;
            unsigned len;
            char hex[13];

            if (event) {
                g_assert_cmpint(sscanf(event, "sgi_kona_pio_write seq %"
                                      SCNu64, &kick), ==, 1);
            }
            event = strstr(lines[i], "sgi_kona_dma_chunk seq ");
            if (!event) {
                continue;
            }
            g_assert_cmpuint(count, <, 2);
            g_assert_cmpint(sscanf(event, "sgi_kona_dma_chunk seq %" SCNu64
                                  " addr 0x%" SCNx64 " len %u hex %12s",
                                  &sequence, &address, &len, hex), ==, 4);
            g_assert_cmpuint(sequence, ==, kick);
            g_assert_cmphex(address, ==, host + io + count * stride);
            g_assert_cmpuint(len, ==, 6);
            g_assert_cmpstr(hex, ==, payload[count]);
            count++;
        }
        g_assert_cmpuint(count, ==, 2);
    }
}

static void test_texture_captured(void)
{
    const unsigned dims[][2] = { { 2, 2 }, { 4, 2 }, { 2, 4 } };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t h[20];

    gl_make_current(s);
    for (unsigned d = 0; d < ARRAY_SIZE(dims); d++) {
        unsigned n = dims[d][0] * dims[d][1];

        memcpy(h, texture_header, sizeof(h));
        h[1] = dims[d][0];
        h[2] = dims[d][1];
        h[13] = h[1] - 1;
        h[14] = h[2] - 1;
        kona_command(s, 0x174, h, 20);
        for (unsigned k = 0; k < n; k += 2) {
            qtest_writeq(s, REG_COMMAND,
                         ((uint64_t)texture_source[k] << 32) |
                         texture_source[k + 1]);
        }
        assert_texture_staging(s, texture_source, n, 0);
        kona_command(s, 0x176, (uint32_t[]){ 0 }, 1);
        assert_native_query_word(s, 0xb92, 0x207, false);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found,
                           "sgi_kona_gl_data token 0x174 data words 4\n"));
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 4 consumed"),
                     ==, 1);
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 8 consumed"),
                     ==, 2);
    g_assert_cmpuint(count_occurrences(found,
                     "token 0x176 args 0x0 0x0 0x0 argc 1\n"), ==, 3);
    g_assert_false(log_shows_misframing(found));
}

static const uint32_t texture_hostile[8] = {
    1, 0xc7, 0x174, 0xc9, 1, 9, 0, 0x33,
};

static void texture_words(QTestState *s, const uint32_t *v, unsigned n,
                          unsigned width)
{
    for (unsigned k = 0; k < n;) {
        if (width && k + 1 < n && (width == 1 || k % 3)) {
            qtest_writeq(s, REG_COMMAND, ((uint64_t)v[k] << 32) | v[k + 1]);
            k += 2;
        } else {
            qtest_writel(s, REG_COMMAND, v[k++]);
        }
    }
}

static void test_texture_widths(void)
{
    const unsigned dims[][2] = { { 2, 2 }, { 4, 2 }, { 2, 4 } };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t h[20], before[7];

    gl_make_current(s);
    gl_window_cleared(s, 0.25, 0.5, 0.75);
    for (unsigned k = 0; k < ARRAY_SIZE(before); k++) {
        before[k] = stencil_colour_or_depth(s, k);
    }
    for (unsigned d = 0; d < ARRAY_SIZE(dims); d++) {
        memcpy(h, texture_header, sizeof(h));
        h[1] = dims[d][0];
        h[2] = dims[d][1];
        /* Opaque words cannot acquire object or layout meaning. */
        h[11] = UINT32_MAX;
        h[12] = 0x174;
        h[13] = 1;
        h[14] = 0xc7;
        for (unsigned width = 0; width < 3; width++) {
            unsigned n = h[1] * h[2];

            prepare_refused_reply(s);
            kona_command(s, 0x174, h, 20);
            texture_words(s, texture_source, n, width);
            assert_texture_staging(s, texture_source, n, 0);
            assert_reply_unchanged(s);
            kona_command(s, 0x174, h, 20);
            texture_words(s, texture_hostile, n - 1, width);
            /* Last payload word and the next header selector share a store. */
            qtest_writeq(s, REG_COMMAND,
                         ((uint64_t)texture_hostile[n - 1] << 32) | 0x174);
            assert_texture_staging(s, texture_hostile, n, 0);
            texture_words(s, h, 20, width);
            texture_words(s, texture_hostile, n - 1, width);
            /* Last payload word and the group tail share a store. */
            qtest_writeq(s, REG_COMMAND,
                         ((uint64_t)texture_hostile[n - 1] << 32) | 0x176);
            qtest_writel(s, REG_COMMAND, 0);
            assert_texture_staging(s, texture_hostile, n, 0);
            assert_reply_unchanged(s);
            for (unsigned k = 0; k < ARRAY_SIZE(before); k++) {
                g_assert_cmphex(stencil_colour_or_depth(s, k), ==, before[k]);
            }
            assert_native_query_word(s, 0xb92, 0x207, width != 0);
        }
    }
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gc-index','value':96}}");
    QDict *reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gl-data-word'}}");
    g_assert_nonnull(qdict_get(reply, "error"));
    qobject_unref(reply);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 4 consumed"),
                     ==, 9);
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 8 consumed"),
                     ==, 18);
    g_assert_cmpuint(count_occurrences(found,
                     "token 0x176 args 0x0 0x0 0x0 argc 1\n"), ==, 9);
}

static void test_texture_reset(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t h[20];

    memcpy(h, texture_header, sizeof(h));
    h[1] = 4;
    for (unsigned k = 0; k < 20; k++) {
        gl_make_current(s);
        kona_command(s, 0x174, h, k);
        qtest_system_reset(s);
        gl_make_current(s);
        assert_native_query_word(s, 0xb92, 0x207, false);
        qtest_system_reset(s);
    }
    for (unsigned k = 0; k < 8; k++) {
        gl_make_current(s);
        kona_command(s, 0x174, h, 20);
        texture_words(s, texture_hostile, k, 2);
        qtest_system_reset(s);
        gl_make_current(s);
        assert_native_query_word(s, 0xb92, 0x207, false);
        qtest_system_reset(s);
    }
    qtest_quit(s);
}

static void test_texture_migrates(void)
{
    uint32_t h[20];

    memcpy(h, texture_header, sizeof(h));
    h[2] = 4;
    for (unsigned split = 0; split < 28; split++) {
        unsigned pos = split < 20 ? 0 : split - 20;
        g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
        g_autofree char *log = NULL, *found = NULL;
        GError *err = NULL;
        QTestState *from, *to;

        dir = g_dir_make_tmp("sn0-kona-texture-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        args = g_strdup_printf(" -incoming %s", uri);
        from = qtest_init(MACHINE_ARGS);
        to = kona_init_logged_args(&log, args);
        gl_make_current(from);
        kona_command(from, 0x174, h, split < 20 ? split : 20);
        texture_words(from, texture_hostile, pos, 2);
        assert_texture_staging(from, texture_hostile, pos, pos);
        sgi_migrate_stopped(from, uri);
        assert_texture_staging(to, texture_hostile, pos, pos);
        for (unsigned which = 0; which < 2; which++) {
            QTestState *s = which ? from : to;

            if (split < 20) {
                texture_words(s, h + split, 20 - split, 2);
            }
            texture_words(s, texture_hostile + pos, 7 - pos, 2);
            qtest_writeq(s, REG_COMMAND,
                         ((uint64_t)texture_hostile[7] << 32) | 0x176);
            qtest_writel(s, REG_COMMAND, 0);
            assert_texture_staging(s, texture_hostile, 8, 0);
            assert_native_query_word(s, 0xb92, 0x207, false);
        }
        qtest_quit(to);
        qtest_quit(from);
        found = kona_log_read(log);
        g_assert_false(log_shows_misframing(found));
        g_assert_cmpuint(count_occurrences(found,
                         "token 0x174 argc 8 consumed"), ==, 1);
        g_assert_cmpuint(count_occurrences(found,
                         "token 0x176 args 0x0 0x0 0x0 argc 1\n"), ==, 1);
        g_rmdir(dir);
    }
}

static void test_texture_refused(void)
{
    const unsigned fields[] = { 0, 4, 6, 7, 8, 9, 15, 17 };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t h[20], before[7];

    gl_make_current(s);
    gl_window_cleared(s, 0.25, 0.5, 0.75);
    for (unsigned k = 0; k < ARRAY_SIZE(before); k++) {
        before[k] = stencil_colour_or_depth(s, k);
    }
    for (unsigned k = 0; k < ARRAY_SIZE(fields) + 1; k++) {
        memcpy(h, texture_header, sizeof(h));
        if (k < ARRAY_SIZE(fields)) {
            h[fields[k]] ^= 1;
        } else {
            return_context_deactivate(s);
        }
        prepare_refused_reply(s);
        kona_command(s, 0x174, h, 20);
        if (k < ARRAY_SIZE(fields) && h[4] == 1 && !h[6]) {
            texture_words(s, texture_source, 4, 0);
        }
        assert_reply_unchanged(s);
        for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
            g_assert_cmphex(stencil_colour_or_depth(s, plane), ==,
                            before[plane]);
        }
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "native texture header refused:"),
                     ==, ARRAY_SIZE(fields) + 1);
    g_assert_cmpuint(count_occurrences(found, "payload length unknown"), ==, 2);
    g_assert_cmpuint(count_occurrences(found, "draining framed rows"),
                     ==, ARRAY_SIZE(fields) - 2);
    g_assert_cmpuint(count_occurrences(found, "DMA source not staged"), ==, 1);
}

static const uint32_t texture_8x8_headers[][20] = {
    { 0xde1, 8, 8, 5, 1, 0, 0, 0, 0, 0x30,
      0, 0, 0, 7, 7, 0, 0x33, 0x2181, 0, 0 },
    { 0xde1, 8, 8, 5, 1, 0, 0, 0, 0, 0x30,
      1, 0, 0, 7, 7, 0, 0x33, 0x2189, 0, 0 },
};

/* Literal independently checked RGBA words from the two public uploads. */
static const uint32_t texture_8x8_sources[][64] = {
    {
        0xff0000ff, 0x000000ff, 0xffffffff, 0x000000ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x00ff00ff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0x0000ffff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffff00ff,
    },
    {
        0xff0000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x000000ff, 0xffffffff, 0x000000ff, 0x00ff00ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0x0000ffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0xffffffff, 0x000000ff, 0xffffffff, 0xffff00ff,
    },
};

static char *renderer_plane_digest(QTestState *s, unsigned plane);

static void texture_plane_digests(QTestState *s, char *digests[7])
{
    for (unsigned plane = 0; plane < 7; plane++) {
        digests[plane] = renderer_plane_digest(s, plane);
    }
}

static void texture_assert_planes(QTestState *s, char *digests[7])
{
    for (unsigned plane = 0; plane < 7; plane++) {
        g_autofree char *after = renderer_plane_digest(s, plane);

        g_assert_cmpstr(after, ==, digests[plane]);
    }
}

static void texture_free_digests(char *digests[7])
{
    for (unsigned plane = 0; plane < 7; plane++) {
        g_free(digests[plane]);
    }
}

static void texture_canaries(QTestState *s, char *digests[7])
{
    gl_make_current_depth(s, 24);
    gl_window_cleared(s, 0.25, 0.5, 0.75);
    kona_command_d(s, 0x1f, (double[]){ 0.625 }, 1);
    kona_command(s, 0x1a, (uint32_t[]){ 0x100 }, 1);
    stencil_config(s, 8);
    stencil_clear(s, 0x5a, 255);
    stencil_config(s, 0);
    texture_plane_digests(s, digests);
    prepare_refused_reply(s);
}

static void test_texture_8x8_captured(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    char *before[7];

    texture_canaries(s, before);
    for (unsigned image = 0; image < 2; image++) {
        for (unsigned width = 0; width < 3; width++) {
            prepare_refused_reply(s);
            kona_command(s, 0x174, texture_8x8_headers[image], 20);
            texture_words(s, texture_8x8_sources[image], 64, width);
            assert_texture_staging(s, texture_8x8_sources[image], 64, 0);
            assert_reply_unchanged(s);
            texture_assert_planes(s, before);
            kona_command(s, 0x176, (uint32_t[]){ image }, 1);
            assert_reply_unchanged(s);
            assert_native_query_word(s, 0xb92, 0x207, width != 0);
        }
    }
    texture_free_digests(before);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 64 consumed"),
                     ==, 6);
}

/* Public PIO sends an opaque one-word record before each captured upload. */
static void test_texture_8x8_native_prefix(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t staged[2][64];
    char *before[7];
    unsigned configs, uploads;

    texture_canaries(s, before);
    for (unsigned image = 0; image < 2; image++) {
        prepare_refused_reply(s);
        qtest_writel(s, REG_COMMAND, 0x1c4);
        qtest_writel(s, REG_COMMAND, image);
        kona_command(s, 0x174, texture_8x8_headers[image], 20);
        texture_words(s, texture_8x8_sources[image], 64, 1);
        assert_texture_staging(s, NULL, 0, 0);
        for (unsigned k = 0; k < 64; k++) {
            QDict *reply;

            qtest_qmp_assert_success(s,
                "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
                "'property':'x-test-gc-index','value':%u}}", k);
            reply = qtest_qmp(s,
                "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
                "'property':'x-test-gl-data-word'}}");
            staged[image][k] = qdict_get_int(reply, "return");
            qobject_unref(reply);
        }
        assert_reply_unchanged(s);
        texture_assert_planes(s, before);
        kona_command(s, 0x176, (uint32_t[]){ image }, 1);
        kona_command(s, 0x177, (uint32_t[]){ 0 }, 1);
        assert_reply_unchanged(s);
        texture_assert_planes(s, before);
        assert_native_query_word(s, 0xb92, 0x207, false);
    }
    texture_free_digests(before);
    qtest_quit(s);
    found = kona_log_read(log);
    configs = count_occurrences(found, "sgi_kona_gl_buffer_config token 0x1 ");
    uploads = count_occurrences(found, "token 0x174 argc 64 consumed");
    /* The canaries set depth once and stencil configuration twice. */
    g_test_message("buffer configuration records: %u (expected 3); "
                   "complete texture uploads: %u (expected 2)",
                   configs, uploads);
    for (unsigned image = 0; image < 2; image++) {
        for (unsigned k = 0; k < 64; k++) {
            if (staged[image][k] != texture_8x8_sources[image][k]) {
                g_test_message("image %u, staged word %u: 0x%08x, expected "
                               "0x%08x", image, k, staged[image][k],
                               texture_8x8_sources[image][k]);
            }
            g_assert_cmphex(staged[image][k], ==,
                            texture_8x8_sources[image][k]);
        }
    }
    g_assert_cmpuint(configs, ==, 3);
    g_assert_cmpuint(uploads, ==, 2);
    g_assert_false(log_shows_misframing(found));
}

static void texture_prefix_upload(QTestState *s, unsigned image)
{
    kona_command(s, 0x174, texture_8x8_headers[image], 20);
    texture_words(s, texture_8x8_sources[image], 64, 1);
    assert_texture_staging(s, texture_8x8_sources[image], 64, 0);
}

/* Captured cleanup must reach the existing reason-post handler. */
static void test_texture_native_cleanup(void)
{
    static const uint32_t cleanup[] = {
        0x177, 0, 0x178, 0, 0x1c5, 0, 0x178, 1, 0x1c5, 1, 0xc7, 0,
    };
    const uint64_t reason_page = 0x300000;
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t reason_before[0x4000], reason_after[0x4000];
    char *before[7];
    unsigned configs, posts;

    kona_command_kicked(s, 0x1f1,
                        (uint32_t[]){ physical_page_word(reason_page),
                                      tagged_offset_word(reason_page),
                                      0x4000 }, 3);
    texture_canaries(s, before);
    texture_prefix_upload(s, 1);
    qtest_memset(s, reason_page, 0xa5, sizeof(reason_before));
    qtest_memset(s, reason_page, 0, 4);
    qtest_memread(s, reason_page, reason_before, sizeof(reason_before));
    for (unsigned k = 0; k < ARRAY_SIZE(cleanup); k++) {
        qtest_writel(s, REG_COMMAND, cleanup[k]);
    }
    assert_reply_unchanged(s);
    texture_assert_planes(s, before);
    assert_texture_staging(s, texture_8x8_sources[1], 64, 0);
    qtest_memread(s, reason_page, reason_after, sizeof(reason_after));
    g_assert_cmpmem(reason_after, sizeof(reason_after),
                    reason_before, sizeof(reason_before));
    kona_command_kicked(s, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    qtest_memread(s, reason_page, reason_after, sizeof(reason_after));
    assert_reply_unchanged(s);
    texture_assert_planes(s, before);
    assert_texture_staging(s, texture_8x8_sources[1], 64, 0);
    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    configs = count_occurrences(found,
                                "sgi_kona_gl_buffer_config token 0x1 ");
    posts = count_occurrences(found, "sgi_kona_reason_post token 0x1f8 ");
    g_test_message("buffer configuration records: %u (expected 3); "
                   "reason posts: %u (expected 1); reason word: 0x%08x",
                   configs, posts, ldl_be_p(reason_after));
    g_assert_cmphex(ldl_be_p(reason_after), ==, 0x103);
    g_assert_cmpmem(reason_after + 4, sizeof(reason_after) - 4,
                    reason_before + 4, sizeof(reason_before) - 4);
    g_assert_cmpuint(configs, ==, 3);
    g_assert_cmpuint(posts, ==, 1);
    assert_native_query_word(s, 0xb92, 0x207, false);
    texture_assert_planes(s, before);
    texture_free_digests(before);
    qtest_quit(s);
    g_clear_pointer(&found, g_free);
    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
}

static void test_texture_prefix_widths(gconstpointer opaque)
{
    uint32_t prefix = GPOINTER_TO_UINT(opaque);
    g_autofree char *log = NULL, *found = NULL;
    g_autofree char *consumed =
        g_strdup_printf("token 0x%x args", prefix);
    QTestState *s = kona_init_logged(&log);
    char *before[7];

    texture_canaries(s, before);
    for (unsigned boundary = 0; boundary < 2; boundary++) {
        for (unsigned image = 0; image < 2; image++) {
            prepare_refused_reply(s);
            if (boundary == 0) {
                qtest_writeq(s, REG_COMMAND, ((uint64_t)prefix << 32) | image);
                texture_prefix_upload(s, image);
            } else {
                qtest_writel(s, REG_COMMAND, prefix);
                qtest_writeq(s, REG_COMMAND, ((uint64_t)image << 32) | 0x174);
                texture_words(s, texture_8x8_headers[image], 20, 1);
                texture_words(s, texture_8x8_sources[image], 64, 1);
                assert_texture_staging(s, texture_8x8_sources[image], 64, 0);
            }
            assert_reply_unchanged(s);
            texture_assert_planes(s, before);
            assert_native_query_word(s, 0xb92, 0x207, false);
        }
    }
    texture_free_digests(before);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_cmpuint(count_occurrences(found,
                     "sgi_kona_gl_buffer_config token 0x1 "), ==, 3);
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 64 consumed"),
                     ==, 4);
    g_assert_cmpuint(count_occurrences(found, consumed),
                     ==, 4);
}

static void test_texture_prefix_reset(gconstpointer opaque)
{
    uint32_t prefix = GPOINTER_TO_UINT(opaque);
    g_autofree char *log = NULL, *found = NULL;
    g_autofree char *consumed =
        g_strdup_printf("token 0x%x args", prefix);
    QTestState *s = kona_init_logged(&log);
    char *before[7];

    texture_canaries(s, before);
    qtest_writel(s, REG_COMMAND, prefix);
    assert_reply_unchanged(s);
    texture_assert_planes(s, before);
    texture_free_digests(before);
    qtest_system_reset(s);
    texture_canaries(s, before);
    texture_prefix_upload(s, 1);
    assert_reply_unchanged(s);
    texture_assert_planes(s, before);
    assert_native_query_word(s, 0xb92, 0x207, false);
    texture_free_digests(before);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, consumed),
                     ==, 0);
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 64 consumed"),
                     ==, 1);
    g_assert_false(log_shows_misframing(found));
}

static void test_texture_prefix_migration(gconstpointer opaque)
{
    uint32_t prefix = GPOINTER_TO_UINT(opaque);
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    g_autofree char *log = NULL, *found = NULL;
    g_autofree char *consumed =
        g_strdup_printf("token 0x%x args", prefix);
    GError *err = NULL;
    QTestState *from, *to;
    char *before[7];

    dir = g_dir_make_tmp("sn0-kona-texture-prefix-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    args = g_strdup_printf(" -incoming %s", uri);
    from = qtest_init(MACHINE_ARGS);
    to = kona_init_logged_args(&log, args);
    texture_canaries(from, before);
    texture_prefix_upload(from, 0);
    assert_reply_unchanged(from);
    qtest_writel(from, REG_COMMAND, prefix);
    sgi_migrate_stopped(from, uri);
    for (unsigned which = 0; which < 2; which++) {
        QTestState *s = which ? from : to;

        assert_reply_unchanged(s);
        texture_assert_planes(s, before);
        assert_texture_staging(s, texture_8x8_sources[0], 64, 0);
        qtest_writeq(s, REG_COMMAND, (UINT64_C(1) << 32) | 0x174);
        texture_words(s, texture_8x8_headers[1], 20, 1);
        texture_words(s, texture_8x8_sources[1], 64, 1);
        assert_texture_staging(s, texture_8x8_sources[1], 64, 0);
        assert_reply_unchanged(s);
        texture_assert_planes(s, before);
        assert_native_query_word(s, 0xb92, 0x207, false);
    }
    texture_free_digests(before);
    qtest_quit(to);
    qtest_quit(from);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found,
                     "sgi_kona_gl_buffer_config token 0x1 "), ==, 0);
    g_assert_cmpuint(count_occurrences(found, consumed),
                     ==, 1);
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 64 consumed"),
                     ==, 1);
    g_assert_false(log_shows_misframing(found));
    g_rmdir(dir);
}

static void test_texture_prefix_handles(gconstpointer opaque)
{
    uint32_t prefix = GPOINTER_TO_UINT(opaque);
    static const uint32_t operands[] = {
        2, 0x174, 0xc7, 0x1c4, 0x1c5, UINT32_MAX,
    };
    g_autofree char *log = NULL, *found = NULL;
    g_autofree char *consumed =
        g_strdup_printf("token 0x%x args", prefix);
    QTestState *s = kona_init_logged(&log);
    char *before[7];

    texture_canaries(s, before);
    texture_prefix_upload(s, 0);
    for (unsigned k = 0; k < ARRAY_SIZE(operands); k++) {
        prepare_refused_reply(s);
        qtest_writeq(s, REG_COMMAND,
                     ((uint64_t)prefix << 32) | operands[k]);
        assert_texture_staging(s, texture_8x8_sources[0], 64, 0);
        assert_reply_unchanged(s);
        texture_assert_planes(s, before);
        assert_native_query_word(s, 0xb92, 0x207, false);
    }
    texture_free_digests(before);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, consumed),
                     ==, ARRAY_SIZE(operands));
    g_assert_cmpuint(count_occurrences(found,
                     "sgi_kona_gl_buffer_config token 0x1 "), ==, 3);
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 64 consumed"),
                     ==, 1);
    g_assert_false(log_shows_misframing(found));
}

static void test_texture_8x8_widths(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t hostile[64];
    char *before[7];

    for (unsigned k = 0; k < 64; k++) {
        hostile[k] = texture_hostile[k % ARRAY_SIZE(texture_hostile)];
    }
    texture_canaries(s, before);
    for (unsigned image = 0; image < 2; image++) {
        for (unsigned width = 0; width < 3; width++) {
            prepare_refused_reply(s);
            kona_command(s, 0x174, texture_8x8_headers[image], 20);
            texture_words(s, hostile, 63, width);
            qtest_writeq(s, REG_COMMAND,
                         ((uint64_t)hostile[63] << 32) | 0x174);
            assert_texture_staging(s, hostile, 64, 0);
            texture_words(s, texture_8x8_headers[1 - image], 20, width);
            texture_words(s, hostile, 63, width);
            qtest_writeq(s, REG_COMMAND,
                         ((uint64_t)hostile[63] << 32) | 0x176);
            qtest_writel(s, REG_COMMAND, 1 - image);
            assert_texture_staging(s, hostile, 64, 0);
            assert_reply_unchanged(s);
            texture_assert_planes(s, before);
            assert_native_query_word(s, 0xb92, 0x207, width != 0);
        }
    }
    texture_free_digests(before);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_cmpuint(count_occurrences(found, "token 0x174 argc 64 consumed"),
                     ==, 12);
}

static void test_texture_8x8_reset(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t hostile[64];

    for (unsigned k = 0; k < 64; k++) {
        hostile[k] = texture_hostile[k % ARRAY_SIZE(texture_hostile)];
    }
    for (unsigned image = 0; image < 2; image++) {
        for (unsigned split = 0; split < 84; split++) {
            char *before[7];
            unsigned pos = split < 20 ? 0 : split - 20;

            texture_canaries(s, before);
            kona_command(s, 0x174, texture_8x8_headers[image],
                          split < 20 ? split : 20);
            texture_words(s, hostile, pos, 2);
            assert_texture_staging(s, hostile, pos, pos);
            assert_reply_unchanged(s);
            texture_assert_planes(s, before);
            texture_free_digests(before);
            qtest_system_reset(s);
            gl_make_current(s);
            assert_texture_staging(s, hostile, 0, 0);
            assert_native_query_word(s, 0xb92, 0x207, false);
            qtest_system_reset(s);
        }
    }
    qtest_quit(s);
}

static void test_texture_8x8_migrates(void)
{
    uint32_t hostile[64];

    for (unsigned k = 0; k < 64; k++) {
        hostile[k] = texture_hostile[k % ARRAY_SIZE(texture_hostile)];
    }
    for (unsigned image = 0; image < 2; image++) {
        for (unsigned split = 0; split < 84; split++) {
            unsigned pos = split < 20 ? 0 : split - 20;
            g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
            g_autofree char *log = NULL, *found = NULL;
            GError *err = NULL;
            QTestState *from, *to;
            char *before[7];

            g_test_message("image %u, partial word %u", image, split);
            dir = g_dir_make_tmp("sn0-kona-texture-8x8-XXXXXX", &err);
            g_assert_no_error(err);
            uri = g_strdup_printf("unix:%s/migrate.sock", dir);
            args = g_strdup_printf(" -incoming %s", uri);
            from = qtest_init(MACHINE_ARGS);
            to = kona_init_logged_args(&log, args);
            texture_canaries(from, before);
            kona_command(from, 0x174, texture_8x8_headers[image],
                          split < 20 ? split : 20);
            texture_words(from, hostile, pos, 2);
            assert_texture_staging(from, hostile, pos, pos);
            assert_reply_unchanged(from);
            texture_assert_planes(from, before);
            sgi_migrate_stopped(from, uri);
            assert_texture_staging(to, hostile, pos, pos);
            for (unsigned which = 0; which < 2; which++) {
                QTestState *s = which ? from : to;

                assert_reply_unchanged(s);
                texture_assert_planes(s, before);
                if (split < 20) {
                    texture_words(s, texture_8x8_headers[image] + split,
                                  20 - split, 2);
                }
                texture_words(s, hostile + pos, 63 - pos, 2);
                qtest_writeq(s, REG_COMMAND,
                             ((uint64_t)hostile[63] << 32) | 0x176);
                qtest_writel(s, REG_COMMAND, image);
                assert_texture_staging(s, hostile, 64, 0);
                assert_reply_unchanged(s);
                texture_assert_planes(s, before);
                assert_native_query_word(s, 0xb92, 0x207, false);
            }
            texture_free_digests(before);
            qtest_quit(to);
            qtest_quit(from);
            found = kona_log_read(log);
            g_assert_false(log_shows_misframing(found));
            g_assert_cmpuint(count_occurrences(found,
                             "token 0x174 argc 64 consumed"), ==, 1);
            g_rmdir(dir);
        }
    }
}

static void test_texture_8x8_refused(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t h[20];
    char *before[7];

    texture_canaries(s, before);
    for (unsigned image = 0; image < 2; image++) {
        for (unsigned field = 0; field < 24; field++) {
            memcpy(h, texture_8x8_headers[image], sizeof(h));
            if (field < 20) {
                h[field] ^= 1;
            } else if (field == 20) {
                h[17] ^= 8; /* Unobserved word-ten/seventeen cross-product. */
            } else if (field == 21) {
                kona_command(s, 0x33, (uint32_t[]){ 0xcf5, 8 }, 2);
            } else if (field == 22) {
                kona_command(s, 0x33, (uint32_t[]){ 0xcf5, 4 }, 2);
                kona_command(s, 0x33, (uint32_t[]){ 0xcf2, 2 }, 2);
            } else {
                kona_command(s, 0x33, (uint32_t[]){ 0xcf2, 0 }, 2);
                return_context_deactivate(s);
            }
            prepare_refused_reply(s);
            kona_command(s, 0x174, h, 20);
            assert_texture_staging(s, NULL, 0, 0);
            assert_reply_unchanged(s);
            texture_assert_planes(s, before);
        }
        gl_make_current_depth(s, 24);
    }
    texture_free_digests(before);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "native texture header refused:"),
                     ==, 48);
    g_assert_null(strstr(found, "sgi_kona_gl_data token 0x174"));
}

static uint64_t renderer_counter(QTestState *s, const char *name)
{
    QDict *reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':%s}}", name);
    uint64_t count = qdict_get_int(reply, "return");

    qobject_unref(reply);
    return count;
}

static char *renderer_plane_digest(QTestState *s, unsigned plane)
{
    QDict *reply;
    char *digest;

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane','value':%u}}", plane);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane-digest'}}");
    digest = g_strdup(qdict_get_str(reply, "return"));
    qobject_unref(reply);
    return digest;
}

static void renderer_fixture(QTestState *s)
{
    gl_make_current_depth(s, 24);
    gl_window_cleared(s, 1, 1, 1);
    kona_command_d(s, 0x1f, (double[]){ 0.625 }, 1);
    kona_command(s, 0x1a, (uint32_t[]){ 0x100 }, 1);
    stencil_config(s, 8);
    stencil_clear(s, 0x5a, 255);
    stencil_config(s, 0);
    kona_command(s, 0xd0, (uint32_t[]){ 0x1701 }, 1);
    kona_command(s, 0xd2, (uint32_t[]){ 0 }, 1);
    kona_command_d(s, 0xd7, (double[]){ 0, 128, 0, 128, -1, 1 }, 6);
    kona_command(s, 0xd0, (uint32_t[]){ 0x1700 }, 1);
    kona_command(s, 0xd2, (uint32_t[]){ 0 }, 1);
    kona_command_f(s, 0x63, (float[]){ 1, 0, 0 }, 3);
    /* Canonical indexed white outside the triangle must remain indexed. */
    kona_command(s, 1, (uint32_t[]){ 3, 0x22, 1020, 1020, 1020 }, 5);
    kona_command(s, 1, (uint32_t[]){ 2, 0x22 }, 2);
    kona_command(s, 1, (uint32_t[]){ 5, PROBE_X, PROBE_Y,
                                    PROBE_X + 127, PROBE_Y + 127 }, 5);
}

static void renderer_triangle(QTestState *s)
{
    gl_begin(s, 4);
    gl_vertex2f(s, 16.25, 16.25);
    gl_vertex2f(s, 111.25, 16.25);
    gl_vertex2f(s, 16.25, 111.25);
    gl_end(s);
}

static void renderer_assert_image(QTestState *s)
{
    g_autofree Expected *expected = g_new(Expected, 1);

    expected_clear(expected, 0xffffff);
    for (unsigned y = 16; y < 128; y++) {
        for (unsigned x = 16; x < 128; x++) {
            if (x + y <= 126) {
                unsigned offset = ((127 - y) * 128 + x) * 3;
                expected->rgb[offset] = 255;
                expected->rgb[offset + 1] = expected->rgb[offset + 2] = 0;
            }
        }
    }
    assert_window_matches(s, expected);
}

static char *renderer_expected_tags(void)
{
    g_autofree uint8_t *tags = g_malloc0(FB_WIDTH * FB_HEIGHT);

    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 128; x++) {
            tags[(PROBE_Y + y) * FB_WIDTH + PROBE_X + x] =
                x >= 16 && y >= 16 && x + y <= 126 ? 0 : 1;
        }
    }
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, tags,
                                        FB_WIDTH * FB_HEIGHT);
}

static void test_renderer_fixture(gconstpointer opaque)
{
    bool gpu = GPOINTER_TO_UINT(opaque);
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    QTestState *s;
    uint32_t depth, stencil;
    g_autofree char *depth_digest = NULL, *stencil_digest = NULL;
    g_autofree char *tag_digest = NULL, *after = NULL;

    if (gpu && !node) {
        g_test_skip("QEMU_TEST_SGI_RENDER_NODE is not set");
        return;
    }
    if (gpu) {
        s = qtest_initf(MACHINE_ARGS " -global sgi-kona.host-gpu=on"
                        " -display egl-headless,rendernode=%s", node);
    } else {
        s = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    }
    renderer_fixture(s);
    depth = stencil_colour_or_depth(s, 5);
    stencil = stencil_colour_or_depth(s, 6);
    g_assert_cmpuint(stencil, ==, 0x5a);
    depth_digest = renderer_plane_digest(s, 5);
    stencil_digest = renderer_plane_digest(s, 6);
    tag_digest = renderer_expected_tags();
    renderer_triangle(s);
    renderer_assert_image(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, gpu ? 1 : 0);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     gpu ? 0 : 1);
    g_assert_cmpuint(stencil_colour_or_depth(s, 5), ==, depth);
    g_assert_cmpuint(stencil_colour_or_depth(s, 6), ==, stencil);
    after = renderer_plane_digest(s, 5);
    g_assert_cmpstr(after, ==, depth_digest);
    g_clear_pointer(&after, g_free);
    after = renderer_plane_digest(s, 6);
    g_assert_cmpstr(after, ==, stencil_digest);
    g_clear_pointer(&after, g_free);
    after = renderer_plane_digest(s, 2);
    g_assert_cmpstr(after, ==, tag_digest);
    renderer_triangle(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, gpu ? 2 : 0);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     gpu ? 0 : 2);
    /* An explicitly excluded dither state executes only through the CPU. */
    kona_command(s, 0x24, (uint32_t[]){ 0xbd0 }, 1);
    renderer_triangle(s);
    renderer_assert_image(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, gpu ? 2 : 0);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     gpu ? 1 : 3);
    qtest_system_reset(s);
    renderer_fixture(s);
    renderer_triangle(s);
    renderer_assert_image(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, gpu ? 3 : 0);
    qtest_quit(s);
}

static void test_renderer_excluded_state(void)
{
    static const uint32_t enables[] = {
        0xb44, 0xbc0, 0xb71, 0xb90, 0xbe2, 0xbf1, 0xbf2, 0xb60,
        0x8037, 0xde1, 0xbd0, 0x3000, 0xc11, 0xb41, 0xb42,
        0xb24, 0xb20, 0xb10, 0xb50,
    };
    static const uint32_t visual_cases[][10] = {
        { UINT32_MAX, 12 }, { 0, 16 }, { 0, 12, 1 }, { 0, 12, 0, 2 },
        { 0, 12, 0, 0, 1 }, { 0, 12, 0, 0, 0, 1 },
        { 0, 12, 0, 0, 0, 0, 16 }, { 0, 12, 0, 0, 0, 0, 24, 8 },
        { 0, 12, 0, 0, 0, 0, 24, 0, 1 },
        { 0, 12, 0, 0, 0, 0, 24, 0, 0, 1 },
    };
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    QTestState *s;
    uint64_t fallbacks = 0;

    if (!node) {
        g_test_skip("QEMU_TEST_SGI_RENDER_NODE is not set");
        return;
    }
    s = qtest_initf(MACHINE_ARGS " -global sgi-kona.host-gpu=on"
                    " -display egl-headless,rendernode=%s", node);
    for (unsigned i = 0; i < ARRAY_SIZE(enables); i++) {
        qtest_system_reset(s);
        renderer_fixture(s);
        kona_command(s, 0x24, &enables[i], 1);
        renderer_triangle(s);
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, 0);
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                         ++fallbacks);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(visual_cases); i++) {
        qtest_system_reset(s);
        renderer_fixture(s);
        g_test_message("excluded visual case %u", i);
        kona_command(s, 1, visual_cases[i], 10);
        renderer_triangle(s);
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, 0);
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                         ++fallbacks);
    }
    qtest_system_reset(s);
    renderer_fixture(s);
    kona_command_f(s, 0x6b, (float[]){ 1, 0, 0, 0.5 }, 4);
    renderer_triangle(s);
    renderer_assert_image(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, 0);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     ++fallbacks);
    qtest_quit(s);
}

static void test_renderer_migration(gconstpointer opaque)
{
    bool source_gpu = GPOINTER_TO_UINT(opaque);
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    g_autofree char *tmpfs = NULL, *uri = NULL;
    g_autofree char *source_args = NULL, *dest_args = NULL;
    char *before[7] = { NULL };
    GError *err = NULL;
    QTestState *from, *to;

    if (!node) {
        g_test_skip("QEMU_TEST_SGI_RENDER_NODE is not set");
        return;
    }
    tmpfs = g_dir_make_tmp("sn0-kona-renderer-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    source_args = source_gpu ? g_strdup_printf(MACHINE_ARGS
        " -global sgi-kona.host-gpu=on -display egl-headless,rendernode=%s",
        node) : g_strdup(MACHINE_ARGS);
    dest_args = source_gpu ?
        g_strdup_printf(MACHINE_ARGS " -incoming %s", uri) :
        g_strdup_printf(MACHINE_ARGS " -incoming %s"
        " -global sgi-kona.host-gpu=on -display egl-headless,rendernode=%s",
        uri, node);
    from = qtest_init(source_args);
    renderer_fixture(from);
    renderer_triangle(from);
    renderer_assert_image(from);
    for (unsigned plane = 0; plane < 7; plane++) {
        before[plane] = renderer_plane_digest(from, plane);
    }
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);
    renderer_assert_image(to);
    for (unsigned plane = 0; plane < 7; plane++) {
        g_autofree char *after = renderer_plane_digest(to, plane);

        g_assert_cmpstr(after, ==, before[plane]);
        g_clear_pointer(&before[plane], g_free);
    }
    g_assert_cmpuint(renderer_counter(to, "x-test-gpu-draws"), ==, 0);
    renderer_triangle(to);
    renderer_assert_image(to);
    g_assert_cmpuint(renderer_counter(to, "x-test-gpu-draws"), ==,
                     source_gpu ? 0 : 1);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

int main(int argc, char **argv)
{
    static const uint32_t bad_format[] = { 0, 0, 1, 1, 5, 1, 0 };
    static const uint32_t bad_type[] = { 0, 0, 1, 1, 4, 6, 0 };
    static const uint32_t too_large[] = { 0, 0, 5457, 1, 4, 1, 0 };
    static const uint32_t huge_width[] = {
        0, 0, UINT32_MAX, UINT32_MAX, 4, 1, 0,
    };

    static const uint32_t raw_enums[] = { 10, 20, 3, 2, 0x1907, 0x1401, 0 };
    static const uint32_t low_bits[] = { 10, 20, 3, 2, 4, 1, 4 };
    static const uint32_t zero_width[] = { 10, 20, 0, 2, 4, 1, 0 };
    static const uint32_t zero_height[] = { 10, 20, 3, 0, 4, 1, 0 };
    static const uint32_t negative_width[] = { 10, 20, -1, 2, 4, 1, 0 };
    static const uint32_t ambiguous[] = { 10, 20, 8, 1, 4, 1, 0 };
    static const uint32_t clipped[] = { 127, 127, 2, 2, 4, 1, 0 };

    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/origin2000/kona-3d/every-command-frames",
                   test_every_command_frames);
    qtest_add_func("sn0/origin2000/kona-3d/packed-forms-frame",
                   test_packed_forms_frame);
    qtest_add_func("sn0/origin2000/kona-3d/light-model-not-bitmap",
                   test_light_model_not_bitmap);
    qtest_add_func("sn0/origin2000/kona-3d/migration-mid-data-run",
                   test_migration_mid_data_run);
    qtest_add_func("sn0/origin2000/kona-3d/ortho-half-size",
                   test_ortho_half_size);
    qtest_add_func("sn0/origin2000/kona-3d/rotated-triangle",
                   test_rotated_triangle);
    qtest_add_func("sn0/origin2000/kona-3d/perspective-quad",
                   test_perspective_quad);
    qtest_add_func("sn0/origin2000/kona-3d/lit-flat-triangle",
                   test_lit_flat_triangle);
    qtest_add_func("sn0/origin2000/kona-3d/packed-colour-across-migration",
                   test_packed_colour_across_migration);
    qtest_add_func("sn0/origin2000/kona-3d/depth-less-and-greater",
                   test_depth_less_and_greater);
    qtest_add_func("sn0/origin2000/kona-3d/depth-without-buffer",
                   test_depth_without_buffer);
    qtest_add_func("sn0/origin2000/kona-3d/blended-triangle",
                   test_blended_triangle);
    qtest_add_func("sn0/origin2000/kona-3d/culled-triangle",
                   test_culled_triangle);
    qtest_add_func("sn0/origin2000/kona-3d/read-pixels", test_read_pixels);
    qtest_add_func("sn0/origin2000/kona-3d/read-capacity",
                   test_read_pixels_capacity);
    qtest_add_func("sn0/origin2000/kona-3d/read-visual-records",
                   test_read_visual_records);
    qtest_add_func("sn0/origin2000/kona-3d/read-ambiguous-level",
                   test_read_ambiguous_level_preserves_source);
    qtest_add_func("sn0/origin2000/kona-3d/read-partial-records",
                   test_read_partial_records);
    qtest_add_func("sn0/origin2000/kona-3d/read-reply-order",
                   test_read_reply_order);
    qtest_add_func("sn0/origin2000/kona-3d/read-coordinates",
                   test_read_coordinates);
    qtest_add_func("sn0/origin2000/kona-3d/read-indexed-source-refused",
                   test_read_indexed_source_refused);
    qtest_add_func("sn0/origin2000/kona-3d/read-double-buffers",
                   test_read_double_buffers);
    qtest_add_func("sn0/origin2000/kona-3d/read-selection",
                   test_read_selection);
    qtest_add_func("sn0/origin2000/kona-3d/get-queries", test_get_queries);
    qtest_add_func("sn0/origin2000/kona-3d/native-depth-state",
                   test_native_depth_state);
    qtest_add_func("sn0/origin2000/kona-3d/native-state-migrates",
                   test_native_state_migrates);
    qtest_add_func("sn0/origin2000/kona-3d/push-and-pop-attrib",
                   test_push_and_pop_attrib);
    qtest_add_func("sn0/origin2000/kona-3d/native-state/bytes",
                   test_native_query_bytes);
    qtest_add_func("sn0/origin2000/kona-3d/native-state/indexed-bits",
                   test_native_indexed_query_bits);
    qtest_add_func("sn0/origin2000/kona-3d/native-state/invalid",
                   test_native_query_invalid);
    qtest_add_func("sn0/origin2000/kona-3d/native-state/attrib",
                   test_native_query_attrib);
    qtest_add_func("sn0/origin2000/kona-3d/native-state/stencil-pixels",
                   test_native_stencil_query_pixels);
    qtest_add_func("sn0/origin2000/kona-3d/native-state/contexts",
                   test_native_query_contexts);
    qtest_add_func("sn0/origin2000/kona-3d/native-state/reset",
                   test_native_query_reset);
    qtest_add_data_func("sn0/origin2000/kona-3d/native-state/migration",
                        GUINT_TO_POINTER(0), test_native_query_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/native-state/"
                        "function-migration",
                        GUINT_TO_POINTER(1), test_native_query_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/native-state/"
                        "operations-migration",
                        GUINT_TO_POINTER(2), test_native_query_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/native-state/enable-migration",
                        GUINT_TO_POINTER(3), test_native_query_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/native-state/default-migration",
                        GUINT_TO_POINTER(4), test_native_query_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/native-state/no-context",
                        GUINT_TO_POINTER(0), test_native_query_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/native-state/missing-page",
                        GUINT_TO_POINTER(1), test_native_query_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/native-state/failed-page",
                        GUINT_TO_POINTER(2), test_native_query_refused);
    qtest_add_func("sn0/origin2000/kona-3d/hints/invalid",
                   test_native_hints_invalid);
    qtest_add_func("sn0/origin2000/kona-3d/hints/attrib",
                   test_native_hints_attrib);
    qtest_add_func("sn0/origin2000/kona-3d/hints/contexts",
                   test_native_hints_contexts);
    qtest_add_func("sn0/origin2000/kona-3d/hints/reset",
                   test_native_hints_reset);
    qtest_add_data_func("sn0/origin2000/kona-3d/hints/migration",
                        GUINT_TO_POINTER(0), test_native_hints_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/hints/"
                        "partial-selector-migration",
                        GUINT_TO_POINTER(1), test_native_hints_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/hints/partial-target-migration",
                        GUINT_TO_POINTER(2), test_native_hints_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/hints/omitted-migration",
                        GUINT_TO_POINTER(3), test_native_hints_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/hints/no-context",
                        GUINT_TO_POINTER(0), test_native_hints_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/hints/missing-page",
                        GUINT_TO_POINTER(1), test_native_hints_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/hints/failed-page",
                        GUINT_TO_POINTER(2), test_native_hints_refused);
    qtest_add_func("sn0/origin2000/kona-3d/hints/basic",
                   test_native_hints);
    qtest_add_func("sn0/origin2000/kona-3d/attrib-depth/basic",
                   test_native_attrib_depth);
    qtest_add_func("sn0/origin2000/kona-3d/attrib-depth/limits",
                   test_native_attrib_depth_limits);
    qtest_add_func("sn0/origin2000/kona-3d/attrib-depth/reset",
                   test_native_attrib_depth_reset);
    qtest_add_func("sn0/origin2000/kona-3d/attrib-depth/contexts",
                   test_native_attrib_depth_contexts);
    qtest_add_data_func("sn0/origin2000/kona-3d/attrib-depth/migrates",
                        GINT_TO_POINTER(false),
                        test_native_attrib_depth_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/attrib-depth/empty-migrates",
                        GINT_TO_POINTER(true),
                        test_native_attrib_depth_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/attrib-depth/no-context",
                        GUINT_TO_POINTER(0), test_native_attrib_depth_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/attrib-depth/missing-page",
                        GUINT_TO_POINTER(1), test_native_attrib_depth_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/attrib-depth/failed-page",
                        GUINT_TO_POINTER(2), test_native_attrib_depth_refused);
    qtest_add_func("sn0/origin2000/kona-3d/attrib-stack-migrates",
                   test_attrib_stack_migrates);


    qtest_add_func("sn0/origin2000/kona-3d/texture/widths",
                   test_texture_widths);
    qtest_add_func("sn0/origin2000/kona-3d/texture/reset",
                   test_texture_reset);
    qtest_add_func("sn0/origin2000/kona-3d/texture/migrates",
                   test_texture_migrates);
    qtest_add_func("sn0/origin2000/kona-3d/texture/refused",
                   test_texture_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/byte/alpha",
                        GUINT_TO_POINTER(0), test_texture_native_byte_formats);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/byte/rgb-to-luminance",
                        GUINT_TO_POINTER(1), test_texture_native_byte_formats);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/byte/luminance",
                        GUINT_TO_POINTER(2), test_texture_native_byte_formats);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/byte/luminance-alpha",
                        GUINT_TO_POINTER(3), test_texture_native_byte_formats);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/byte/migrate-rows",
                        GUINT_TO_POINTER(2), test_texture_byte_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/byte/migrate-complete",
                        GUINT_TO_POINTER(5), test_texture_byte_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/contiguous",
                        GUINT_TO_POINTER(0), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/strided",
                        GUINT_TO_POINTER(1), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/unmapped",
                        GUINT_TO_POINTER(2), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/short",
                        GUINT_TO_POINTER(3), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/extra",
                        GUINT_TO_POINTER(4), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/prefix-mismatch",
                        GUINT_TO_POINTER(5), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/chunk-boundary",
                        GUINT_TO_POINTER(6), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/migrate-pending",
                        GUINT_TO_POINTER(7), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/migrate-complete",
                        GUINT_TO_POINTER(8), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/reset",
                        GUINT_TO_POINTER(9), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/overlap-refused",
                        GUINT_TO_POINTER(10), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/page-boundary",
                        GUINT_TO_POINTER(11), test_texture_native_dma);
    qtest_add_func("sn0/origin2000/kona-3d/texture/native-draw",
                   test_texture_native_draw);
    qtest_add_func("sn0/origin2000/kona-3d/texture/native-rows",
                   test_texture_native_rows);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/float/pio",
                        GUINT_TO_POINTER(0), test_texture_native_float);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/float/dma",
                        GUINT_TO_POINTER(1), test_texture_native_float);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/float/nonfinite",
                        GUINT_TO_POINTER(2), test_texture_native_float);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/float/"
                        "migration/partial-pio",
                        GUINT_TO_POINTER(0), test_texture_float_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/float/"
                        "migration/complete-pio",
                        GUINT_TO_POINTER(1), test_texture_float_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/float/"
                        "migration/pending-dma",
                        GUINT_TO_POINTER(2), test_texture_float_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/float/"
                        "migration/complete-dma",
                        GUINT_TO_POINTER(3), test_texture_float_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/dma/trace-source",
                        GUINT_TO_POINTER(12), test_texture_native_dma);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/native-migrate-rows",
                        GUINT_TO_POINTER(2), test_texture_native_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture/"
                        "native-migrate-complete", GUINT_TO_POINTER(4),
                        test_texture_native_migrates);
    qtest_add_func("sn0/origin2000/kona-3d/texture/publication",
                   test_texture_native_publication);
    qtest_add_func("sn0/origin2000/kona-3d/texture/captured",
                   test_texture_captured);
    qtest_add_func("sn0/origin2000/kona-3d/unmodeled-queries-refused",
                   test_unmodeled_queries_refused);
    qtest_add_func("sn0/origin2000/kona-3d/unknown-query-refused",
                   test_unknown_query_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-format-refused",
                        bad_format, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-type-refused",
                        bad_type, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-size-refused",
                        too_large, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-huge-width-refused",
                        huge_width, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-raw-enums-refused",
                        raw_enums, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-low-bits-refused",
                        low_bits, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-zero-width-refused",
                        zero_width, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-zero-height-refused",
                        zero_height, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-negative-width-refused",
                        negative_width, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-ambiguous-extent-refused",
                        ambiguous, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-clipped-refused",
                        clipped, test_read_pixels_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-missing-page-refused",
                        GINT_TO_POINTER(false), test_read_destination_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-failed-page-refused",
                        GINT_TO_POINTER(true), test_read_destination_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/push-texture-refused",
                        GUINT_TO_POINTER(0x40000), test_push_attrib_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/push-transform-refused",
                        GUINT_TO_POINTER(0x1000), test_push_attrib_refused);
    qtest_add_func("sn0/origin2000/kona-3d/reply-without-context-refused",
                   test_reply_without_context_refused);

    qtest_add_func("sn0/origin2000/kona-3d/current-colour-rgb",
                   test_current_colour_rgb);
    qtest_add_func("sn0/origin2000/kona-3d/return-context-isolation",
                   test_return_context_isolation);
    qtest_add_data_func("sn0/origin2000/kona-3d/invalid-draw-migrates",
                        GUINT_TO_POINTER(1), test_return_source_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/invalid-read-migrates",
                        GUINT_TO_POINTER(4), test_return_source_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/unknown-binding-migrates",
                        GUINT_TO_POINTER(0x114), test_return_source_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/partial-draw-migrates",
                        GUINT_TO_POINTER(0x1001), test_return_source_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/partial-read-migrates",
                        GUINT_TO_POINTER(0x1004), test_return_source_migrates);
    qtest_add_data_func("sn0/origin2000/kona-3d/partial-binding-migrates",
                        GUINT_TO_POINTER(0x1114), test_return_source_migrates);

    qtest_add_data_func("sn0/origin2000/kona-3d/read-destroyed-replaced",
                        GINT_TO_POINTER(false),
                        test_read_destroyed_window_replaced);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-replaced-migrates",
                        GINT_TO_POINTER(true),
                        test_read_destroyed_window_replaced);

    qtest_add_data_func("sn0/origin2000/kona-3d/retired-invalid-draw",
                        GUINT_TO_POINTER(1), test_read_retired_state);
    qtest_add_data_func("sn0/origin2000/kona-3d/retired-invalid-read",
                        GUINT_TO_POINTER(4), test_read_retired_state);
    qtest_add_data_func("sn0/origin2000/kona-3d/retired-unknown-binding",
                        GUINT_TO_POINTER(0x114), test_read_retired_state);
    qtest_add_func("sn0/origin2000/kona-3d/placed-move-keeps-swap",
                   test_read_placed_move_keeps_swap);

    qtest_add_data_func("sn0/origin2000/kona-3d/read-single-retired-replaced",
                        GINT_TO_POINTER(false),
                        test_read_single_retired_replaced);
    qtest_add_data_func("sn0/origin2000/kona-3d/read-single-retired-migrates",
                        GINT_TO_POINTER(true),
                        test_read_single_retired_replaced);

    qtest_add_func("sn0/origin2000/kona-3d/stencil/visual-present-and-absent",
                   test_stencil_visual_present_and_absent);

    qtest_add_func("sn0/origin2000/kona-3d/stencil/comparison-order-and-mask",
                   test_stencil_comparison_order_and_mask);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/operation-range",
                   test_stencil_operation_range);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/outcome-operations",
                   test_stencil_outcome_operations);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/only-clear",
                   test_stencil_only_clear);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/alpha-and-disabled-tests",
                   test_stencil_alpha_and_disabled_tests);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/masks-and-colour-targets",
                   test_stencil_masks_and_colour_targets);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/"
                   "incomplete-or-invalid-visual-refused",
                   test_stencil_incomplete_or_invalid_visual_refused);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/clear-scissor-and-clip",
                   test_stencil_clear_scissor_and_clip);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/context-and-attrib",
                   test_stencil_context_and_attrib);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/reset-and-partial-packets",
                   test_stencil_reset_and_partial_packets);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/state-and-plane-migrate",
                   test_stencil_state_and_plane_migrate);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/"
                   "visual-readback-remains-refused",
                   test_stencil_visual_readback_remains_refused);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/completion/"
                   "partial-visual-migration",
                   test_stencil_partial_visual_migration);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/completion/"
                   "accumulation-clear-diagnostic",
                   test_stencil_accumulation_clear_diagnostic);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/completion/"
                   "invalid-state-enumerants",
                   test_stencil_invalid_state_enumerants);
    qtest_add_func("sn0/origin2000/kona-3d/stencil/completion/"
                   "destroyed-context-pixels-retained",
                   test_stencil_destroyed_context_pixels_retained);
    qtest_add_func("sn0/origin2000/kona-3d/depth-blend/"
                   "depth-clear-preserves-colour",
                   test_depth_clear_preserves_colour);
    qtest_add_func("sn0/origin2000/kona-3d/depth-blend/"
                   "colour-masks-preserve-channels",
                   test_colour_masks_preserve_channels);
    qtest_add_func("sn0/origin2000/kona-3d/depth-blend/"
                   "depth-plane-migration",
                   test_depth_plane_migration);
    qtest_add_func("sn0/origin2000/kona-3d/depth-blend/"
                   "blend-without-alpha-plane",
                   test_blend_without_alpha_plane);
    qtest_add_func("sn0/origin2000/kona-3d/texture-8x8/captured",
                   test_texture_8x8_captured);
    qtest_add_func("sn0/origin2000/kona-3d/texture-8x8/native-prefix",
                   test_texture_8x8_native_prefix);
    qtest_add_func("sn0/origin2000/kona-3d/texture-prefix/native-cleanup",
                   test_texture_native_cleanup);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture-prefix/widths",
                        GUINT_TO_POINTER(0x1c4), test_texture_prefix_widths);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture-cleanup-prefix/widths",
                        GUINT_TO_POINTER(0x1c5), test_texture_prefix_widths);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture-prefix/reset",
                        GUINT_TO_POINTER(0x1c4), test_texture_prefix_reset);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture-cleanup-prefix/reset",
                        GUINT_TO_POINTER(0x1c5), test_texture_prefix_reset);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture-prefix/migration",
                        GUINT_TO_POINTER(0x1c4), test_texture_prefix_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture-cleanup-prefix/"
                        "migration",
                        GUINT_TO_POINTER(0x1c5), test_texture_prefix_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture-prefix/handles",
                        GUINT_TO_POINTER(0x1c4),
                        test_texture_prefix_handles);
    qtest_add_data_func("sn0/origin2000/kona-3d/texture-cleanup-prefix/"
                        "handles",
                        GUINT_TO_POINTER(0x1c5),
                        test_texture_prefix_handles);
    qtest_add_func("sn0/origin2000/kona-3d/texture-8x8/widths",
                   test_texture_8x8_widths);
    qtest_add_func("sn0/origin2000/kona-3d/texture-8x8/reset",
                   test_texture_8x8_reset);
    qtest_add_func("sn0/origin2000/kona-3d/texture-8x8/migration",
                   test_texture_8x8_migrates);
    qtest_add_func("sn0/origin2000/kona-3d/texture-8x8/refused",
                   test_texture_8x8_refused);
    qtest_add_data_func("sn0/origin2000/kona-3d/renderer/real-gl",
                        GUINT_TO_POINTER(1), test_renderer_fixture);
    qtest_add_data_func("sn0/origin2000/kona-3d/renderer/missing-provider",
                        GUINT_TO_POINTER(0), test_renderer_fixture);
    qtest_add_data_func("sn0/origin2000/kona-3d/renderer/gpu-to-cpu-migration",
                        GUINT_TO_POINTER(1), test_renderer_migration);
    qtest_add_data_func("sn0/origin2000/kona-3d/renderer/cpu-to-gpu-migration",
                        GUINT_TO_POINTER(0), test_renderer_migration);
    qtest_add_func("sn0/origin2000/kona-3d/renderer/excluded-state",
                   test_renderer_excluded_state);
    qts = qtest_init(MACHINE_ARGS);
    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
