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
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_posted(s);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, fbits(0.125));
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
    sgi_migrate_stopped(from, uri);

    qtest_memset(to, RETURN_PAGE, 0, 64);
    kona_command(to, 0xc8, (uint32_t[]){ 0 }, 1);
    kona_command(to, 0x4c, (uint32_t[]){ 0xb00 }, 1);
    assert_reply_posted(to);
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 8), ==, fbits(0.25));
    g_assert_cmphex(guest_word(to, RETURN_PAGE + 12), ==, fbits(0.5));

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
    qtest_add_func("sn0/origin2000/kona-3d/attrib-stack-migrates",
                   test_attrib_stack_migrates);


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
    qts = qtest_init(MACHINE_ARGS);
    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
