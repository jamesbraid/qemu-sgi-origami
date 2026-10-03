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

/*
 * glReadPixels' 0x39 of the 3 by 2 pixels at window (10, 20), straddling
 * the rectangle's lower-left corner: GL_RGBA and GL_UNSIGNED_BYTE come
 * back from byte 8 of the return page, bottom row first, alpha 255; then
 * GL_RGB, whose 9-byte rows pad to 12 under the default pack alignment
 * of 4. A model that answers nothing leaves word 0 clear and the client
 * waiting.
 */
static void test_read_pixels(void)
{
    static const uint32_t rgba[6] = {
        0x0000ffff, 0x0000ffff, 0x0000ffff,
        0x0000ffff, 0xff0000ff, 0xff0000ff,
    };
    static const uint8_t rgb[21] = {
        0, 0, 0xff, 0, 0, 0xff, 0, 0, 0xff, 0, 0, 0,
        0, 0, 0xff, 0xff, 0, 0, 0xff, 0, 0,
    };
    uint8_t got[21];

    gl_reset();
    gl_make_current(qts);
    gl_rect_scene(qts);
    qtest_memset(qts, RETURN_PAGE + 8, 0xa5, 64);
    kona_command(qts, 0x39, (uint32_t[]){ 10, 20, 3, 2, 0x1908, 0x1401, 0 },
                 7);
    assert_reply_posted(qts);
    for (int k = 0; k < 6; k++) {
        g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8 + 4 * k), ==,
                        rgba[k]);
    }

    qtest_memset(qts, RETURN_PAGE + 8, 0, 64);
    kona_command(qts, 0x39, (uint32_t[]){ 10, 20, 3, 2, 0x1907, 0x1401, 0 },
                 7);
    assert_reply_posted(qts);
    qtest_memread(qts, RETURN_PAGE + 8, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), rgb, sizeof(rgb));
}

/*
 * glGet's 0x4c: the modelview matrix glLoadMatrixf set, as sixteen float
 * words from byte 8; the viewport as four integers; the depth range
 * glDepthRange set as two floats; and GL_DEPTH_TEST's enable as 1. A name
 * the model does not keep, GL_MAX_TEXTURE_SIZE, still posts word 0 and
 * leaves the page's words as they were.
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
    kona_command_d(qts, 0xcf, (double[]){ 0.25, 0.75 }, 2);
    kona_command(qts, 0x24, (uint32_t[]){ 0xb71 }, 1);

    kona_command(qts, 0x4c, (uint32_t[]){ 0xba6 }, 1);
    assert_reply_posted(qts);
    for (int k = 0; k < 16; k++) {
        g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8 + 4 * k), ==, m[k]);
    }
    kona_command(qts, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 0);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 12), ==, 0);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 16), ==, 128);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 20), ==, 128);
    kona_command(qts, 0x4c, (uint32_t[]){ 0xb70 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, fbits(0.25));
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 12), ==, fbits(0.75));
    kona_command(qts, 0x4c, (uint32_t[]){ 0xb71 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 1);

    qtest_memset(qts, RETURN_PAGE + 8, 0xa5, 4);
    kona_command(qts, 0x4c, (uint32_t[]){ 0xd33 }, 1);
    assert_reply_posted(qts);
    g_assert_cmphex(guest_word(qts, RETURN_PAGE + 8), ==, 0xa5a5a5a5);
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

int main(int argc, char **argv)
{
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
    qtest_add_func("sn0/origin2000/kona-3d/get-queries", test_get_queries);
    qtest_add_func("sn0/origin2000/kona-3d/push-and-pop-attrib",
                   test_push_and_pop_attrib);
    qtest_add_func("sn0/origin2000/kona-3d/attrib-stack-migrates",
                   test_attrib_stack_migrates);

    qts = qtest_init(MACHINE_ARGS);
    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
