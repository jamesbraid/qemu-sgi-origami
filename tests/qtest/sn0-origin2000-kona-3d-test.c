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
#include "migration/savevm.h"
#include <float.h>
#include <math.h>
#include "sgi-migrate.h"
#include "sgi-test.h"
#include "prereq.h"

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
    { 0x176, 1 }, { 0x178, 1 }, { 0x179, 22 }, { 0x17d, 6 }, { 0x1d5, 5 },
    { 0x106, 5 }, { 0x17b, 14 }, { 0x17c, 4 }, { 0xed, 4 },
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

static void curve_select(QTestState *s, unsigned slot, unsigned coefficient)
{
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-x','value':%u}}", slot);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-y','value':%u}}", coefficient);
}

static bool curve_valid(QTestState *s, unsigned slot)
{
    QDict *reply;
    bool valid;

    curve_select(s, slot, 0);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-texture-curve-valid'}}");
    valid = qdict_get_bool(reply, "return");
    qobject_unref(reply);
    return valid;
}

static double curve_value(QTestState *s, unsigned slot, unsigned coefficient)
{
    QDict *reply;
    double value;

    curve_select(s, slot, coefficient);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-texture-curve'}}");
    value = qdict_get_double(reply, "return");
    qobject_unref(reply);
    return value;
}

/* Four signed mantissas and two-bit exponents occupy each 40-bit group. */
static void curve_fixture(uint32_t words[33], double values[64], unsigned seed)
{
    for (unsigned group = 0; group < 16; group++) {
        uint64_t packed = 0;

        for (unsigned j = 0; j < 4; j++) {
            unsigned i = group * 4 + j;
            int mantissa = (int)((i + seed) % 64) - 32;
            unsigned exponent = (i + seed) % 4;
            uint32_t code = ((uint32_t)(uint8_t)mantissa << 2) | exponent;

            packed |= (uint64_t)code << (10 * j);
            values[i] = mantissa * (1 << exponent) / 128.0;
        }
        words[group * 2] = packed;
        words[group * 2 + 1] = packed >> 32;
    }
    words[32] = 0x17e; /* Transport padding cannot open another command. */
}

static void curve_assert(QTestState *s, unsigned slot, const double values[64])
{
    g_assert_true(curve_valid(s, slot));
    for (unsigned i = 0; i < 64; i++) {
        g_assert_cmpfloat(curve_value(s, slot, i), ==, values[i]);
    }
}

static void curve_words(QTestState *s, const uint32_t *words, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        qtest_writel(s, REG_COMMAND, words[i]);
    }
}

static void curve_open(QTestState *s, unsigned slot)
{
    kona_command(s, 0x17e, (uint32_t[]){ slot }, 1);
}

static void test_texture_curve_transport(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t words[33], replacement[33], other[33];
    double values[64], next[64], separate[64];

    gl_make_current(s);
    curve_fixture(words, values, 0);
    curve_fixture(replacement, next, 11);
    curve_fixture(other, separate, 23);
    curve_open(s, 7);
    /* Sixteen doubleword stores carry coefficients, then one word padding. */
    for (unsigned pair = 0; pair < 16; pair++) {
        qtest_writeq(s, REG_COMMAND,
                      ((uint64_t)words[2 * pair] << 32) | words[2 * pair + 1]);
        g_assert_false(curve_valid(s, 7));
    }
    curve_words(s, words + 32, 1);
    curve_assert(s, 7, values);
    curve_open(s, 8);
    curve_words(s, other, 33);
    curve_open(s, 7);
    curve_words(s, replacement, 33);
    curve_assert(s, 7, next);
    curve_assert(s, 8, separate);

    /* A malformed reserved bit cannot replace the previously valid slot. */
    words[1] |= 0x100;
    curve_open(s, 7);
    curve_words(s, words, 33);
    curve_assert(s, 7, next);
    curve_assert(s, 8, separate);
    curve_open(s, 4096);
    curve_words(s, replacement, 33);
    g_assert_false(curve_valid(s, 4096));
    curve_assert(s, 7, next);
    assert_textport_in_step(s);
    qtest_system_reset(s);
    g_assert_false(curve_valid(s, 7));
    g_assert_false(curve_valid(s, 8));
    qtest_quit(s);
}

static void test_texture_curve_migration(gconstpointer opaque)
{
    unsigned consumed = GPOINTER_TO_UINT(opaque);
    uint32_t words[33], replacement[33];
    double values[64], next[64];
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("kona-curve-migration-XXXXXX",
                                        &error);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    QTestState *from, *to;

    g_assert_no_error(error);
    curve_fixture(words, values, 0);
    curve_fixture(replacement, next, 11);
    from = qtest_init(MACHINE_ARGS);
    gl_make_current(from);
    curve_open(from, 7);
    curve_words(from, words, 33);
    curve_open(from, 7);
    curve_words(from, replacement, consumed);
    curve_assert(from, 7, consumed == 33 ? next : values);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    curve_assert(to, 7, consumed == 33 ? next : values);
    curve_words(to, replacement + consumed, 33 - consumed);
    curve_assert(to, 7, next);
    assert_textport_in_step(to);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
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
    x_read_fixture_command(qts, 0x129,
                            (uint32_t[]) { 0, 0, 0, 0, 0xf00fff, 0,
                                           0x180c, 0x40001, 1, 6, 5, 1, 1 },
                            13);
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
    g_assert_cmpuint(qdict_get_int(reply, "return"), ==, 3);
    qobject_unref(reply);
    for (unsigned format = 4; format <= 5; format++) {
        static const uint32_t types[] = { 1, 3, 6 };

        for (unsigned k = 0; k < ARRAY_SIZE(types); k++) {
            prepare_refused_reply(qts);
            kona_command(qts, 0x39,
                         (uint32_t[]) { 0, 0, 3, 2, format, types[k], 0 }, 7);
            assert_reply_unchanged(qts); /* Last source pixel is indexed. */
        }
    }
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
    qtest_memset(s, RETURN_PAGE + 4, 0xa5, 4 * count + 8);
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

    uint32_t actual = (image[at] << 16) | (image[at + 1] << 8) | image[at + 2];

    g_assert_cmphex(actual, ==, rgb);
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

/* All staged words, eight hex digits each: x-test-gl-data-word in bulk. */
static char *staged_words(QTestState *s)
{
    QDict *reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gl-data-words'}}");
    char *words = g_strdup(qdict_get_str(reply, "return"));

    qobject_unref(reply);
    return words;
}

static uint32_t staged_word_at(const char *words, unsigned k)
{
    char digits[9] = { 0 };

    g_assert_cmpuint(strlen(words), >=, (k + 1) * 8);
    memcpy(digits, words + k * 8, 8);
    return g_ascii_strtoull(digits, NULL, 16);
}

static void assert_texture_staging(QTestState *s, const uint32_t *v,
                                   unsigned n, unsigned have)
{
    QDict *reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gl-data-have'}}");
    g_autofree char *words = NULL;

    g_assert_cmpuint(qdict_get_int(reply, "return"), ==, have);
    qobject_unref(reply);
    words = staged_words(s);
    for (unsigned k = 0; k < n; k++) {
        g_assert_cmphex(staged_word_at(words, k), ==, v[k]);
    }
}

static void texture_native_bind(QTestState *s, uint32_t handle)
{
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };

    kona_command(s, 0x1d5,
                 (uint32_t[]) { handle, (2u << 28) | (41 << 12),
                                0x30 << 14, 0x11, 1 }, 5);
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

static const uint32_t texture_8x8_headers[2][20];
static const uint32_t texture_8x8_sources[2][64];
static void texture_words(QTestState *s, const uint32_t *v, unsigned n,
                          unsigned width);

static void texture_rebind_uploads(QTestState *s, unsigned mode)
{
    bool reverse = mode == 1, wide = mode != 0;
    uint32_t h[20];

    gl_make_current(s);
    for (unsigned upload = 0; upload < 2; upload++) {
        unsigned object = reverse ? 1 - upload : upload;
        uint32_t handle = reverse ? (object ? 17 : 29) :
                                  (object ? 29 : 17);

        memcpy(h, wide ? texture_8x8_headers[object] : texture_header,
               sizeof(h));
        h[10] = handle;
        if (!wide) {
            texture_native_bind(s, handle);
        }
        kona_command(s, 0x174, h, 20);
        if (wide) {
            texture_words(s, texture_8x8_sources[object], 64, 1);
        } else {
            for (unsigned k = 0; k < 4; k++) {
                qtest_writel(s, REG_COMMAND,
                             object ? 0x00ff00ff : texture_source[k]);
            }
        }
        kona_command(s, 0x176, (uint32_t[]) { handle }, 1);
        if (!wide) {
            assert_texture_pixel(s, 0, 0,
                                 object ? 0x00ff00ff : texture_source[0]);
        }
    }
}

static void texture_rebind_packet_layout(QTestState *s, uint32_t selector,
                                         uint32_t handle, bool wide)
{
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };

    record[3] = wide ? 0x33 : 0;
    kona_command(s, 0x178, &selector, 1);
    kona_command(s, 0x106,
                 (uint32_t[]) { handle, (2u << 28) | (41 << 12),
                                0x30 << 14, wide ? 0x33 : 0x11, 1 }, 5);
    kona_command(s, 0x179, record, ARRAY_SIZE(record));
}

static void texture_rebind_packet(QTestState *s, uint32_t selector,
                                  uint32_t handle)
{
    texture_rebind_packet_layout(s, selector, handle, false);
}

static void test_texture_rebind_control(gconstpointer opaque)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    QTestState *s = qtest_init(MACHINE_ARGS);
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool reverse = mode == 1, wide = mode != 0;
    uint32_t a = reverse ? 29 : 17, b = reverse ? 17 : 29;
    uint32_t sample_a = wide ? 0xffffffff : texture_source[0];
    uint32_t sample_b = wide ? 0x000000ff : 0x00ff00ff;

    texture_rebind_uploads(s, mode);
    gl_window_cleared(s, 0, 0, 1);
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    texture_rebind_packet_layout(s, 1, a, wide);
    assert_texture_pixel(s, wide ? 2 : 0, wide ? 2 : 0, sample_a);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, sample_a >> 8);
    texture_rebind_packet_layout(s, 0, b, wide);
    assert_texture_pixel(s, wide ? 2 : 0, wide ? 2 : 0, sample_b);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, sample_b >> 8);
    texture_rebind_packet_layout(s, 1, UINT32_MAX, wide);
    assert_texture_pixel(s, wide ? 2 : 0, wide ? 2 : 0, sample_a);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, sample_a >> 8);
    texture_rebind_packet_layout(s, 0, UINT32_MAX, wide);
    assert_texture_pixel(s, wide ? 2 : 0, wide ? 2 : 0, sample_b);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, sample_b >> 8);
    qtest_quit(s);
}

static void test_texture_rebind_refusals(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    texture_rebind_uploads(s, false);
    texture_native_bind(s, 17);
    assert_texture_pixel(s, 0, 0, texture_source[0]);
    kona_command(s, 0x178, (uint32_t[]) { 29 }, 1);
    assert_texture_pixel(s, 0, 0, 0x00ff00ff);
    texture_rebind_packet(s, 1, UINT32_MAX);
    assert_texture_pixel(s, 0, 0, 0);
    texture_rebind_packet(s, 2, UINT32_MAX);
    assert_texture_pixel(s, 0, 0, 0);

    texture_rebind_packet(s, 1, 17);
    assert_texture_pixel(s, 0, 0, texture_source[0]);
    kona_command(s, 0x1c5, (uint32_t[]) { 17 }, 1);
    texture_rebind_packet(s, 1, UINT32_MAX);
    assert_texture_pixel(s, 0, 0, 0);
    qtest_quit(s);
}

static void test_texture_rebind_missing_draw(void)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    texture_rebind_uploads(s, false);
    gl_window_cleared(s, 0, 0, 1);
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    texture_rebind_packet(s, 1, 17);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0xff0000);

    gl_window_cleared(s, 0, 0, 1);
    texture_rebind_packet(s, 0, UINT32_MAX);
    assert_texture_pixel(s, 0, 0, 0);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0x0000ff);

    texture_rebind_packet(s, 1, 17);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0xff0000);
    gl_window_cleared(s, 0, 0, 1);
    texture_rebind_packet(s, 2, UINT32_MAX);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0x0000ff);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "texture rebind without an "
                                       "associated selector; draw refused"),
                     ==, 2);
}

static void test_texture_rebind_missing_migration(void)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    QTestState *from = qtest_init(MACHINE_ARGS), *to;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    texture_rebind_uploads(from, false);
    gl_window_cleared(from, 0, 0, 1);
    kona_command(from, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(from, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(from, 0x80, (float[]) { 0.25, 0.25 }, 2);
    texture_rebind_packet(from, 0, UINT32_MAX);
    dir = g_dir_make_tmp("sn0-kona-rebind-missing-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    gl_triangle_z(to, tri, 0);
    assert_stencil_colour(to, 0x0000ff);
    texture_rebind_packet(to, 1, 17);
    gl_triangle_z(to, tri, 0);
    assert_stencil_colour(to, 0xff0000);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_texture_rebind_recovery_migration(void)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };
    QTestState *from = qtest_init(MACHINE_ARGS), *to;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    texture_rebind_uploads(from, false);
    gl_window_cleared(from, 0, 0, 1);
    kona_command(from, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(from, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(from, 0x80, (float[]) { 0.25, 0.25 }, 2);
    texture_rebind_packet(from, 0, UINT32_MAX);
    kona_command(from, 0x178, (uint32_t[]) { 1 }, 1);
    kona_command(from, 0x106,
                 (uint32_t[]) { 17, (2u << 28) | (41 << 12),
                                0x30 << 14, 0x11, 1 }, 5);
    dir = g_dir_make_tmp("sn0-kona-rebind-recovery-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    kona_command(to, 0x179, record, ARRAY_SIZE(record));
    gl_triangle_z(to, tri, 0);
    assert_stencil_colour(to, 0xff0000);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_texture_rebind_refusal_lifetime(gconstpointer opaque)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    unsigned kind = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);

    texture_rebind_uploads(s, false);
    gl_window_cleared(s, 0, 0, 1);
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    texture_rebind_packet(s, 0, UINT32_MAX);
    if (kind == 1) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        texture_rebind_packet(s, 1, 17);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    } else if (kind == 2) {
        uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };

        kona_command(s, 0x178, (uint32_t[]) { 1 }, 1);
        kona_command(s, 0x106,
                     (uint32_t[]) { 17, (2u << 28) | (41 << 12),
                                    0x30 << 14, 0x11, 1 }, 5);
        kona_command(s, 0xc7, (uint32_t[]) { 0 }, 1);
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
    } else {
        texture_rebind_packet(s, 1, 0xdeadbeef);
    }
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0x0000ff);
    qtest_quit(s);
}

static void test_texture_rebind_standalone_recovery(void)
{
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    QTestState *s = qtest_init(MACHINE_ARGS);

    texture_rebind_uploads(s, false);
    gl_window_cleared(s, 0, 0, 1);
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    texture_rebind_packet(s, 0, UINT32_MAX);
    kona_command(s, 0x178, (uint32_t[]) { 17 }, 1);
    assert_texture_pixel(s, 0, 0, texture_source[0]);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0xff0000);
    qtest_quit(s);
}

static void test_texture_rebind_contexts(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    texture_rebind_uploads(s, false);
    texture_rebind_packet(s, 1, 17);
    assert_texture_pixel(s, 0, 0, texture_source[0]);
    return_context_deactivate(s);
    attrib_context_fresh(s, SAVE_AREA + 0x20000);
    texture_rebind_packet(s, 1, UINT32_MAX);
    assert_texture_pixel(s, 0, 0, 0);
    return_context_deactivate(s);
    return_context_restore(s, SAVE_AREA);
    texture_rebind_packet(s, 1, UINT32_MAX);
    assert_texture_pixel(s, 0, 0, texture_source[0]);
    qtest_quit(s);
}

static void test_texture_rebind_reset(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    texture_rebind_uploads(s, false);
    texture_rebind_packet(s, 1, 17);
    assert_texture_pixel(s, 0, 0, texture_source[0]);
    qtest_system_reset(s);
    texture_rebind_uploads(s, false);
    texture_rebind_packet(s, 1, UINT32_MAX);
    assert_texture_pixel(s, 0, 0, 0);
    qtest_quit(s);
}

static void test_texture_rebind_migration(void)
{
    QTestState *from = qtest_init(MACHINE_ARGS), *to;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    texture_rebind_uploads(from, false);
    texture_rebind_packet(from, 1, 17);
    texture_rebind_packet(from, 0, 29);
    dir = g_dir_make_tmp("sn0-kona-rebind-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    texture_rebind_packet(to, 1, UINT32_MAX);
    assert_texture_pixel(to, 0, 0, texture_source[0]);
    texture_rebind_packet(to, 0, UINT32_MAX);
    assert_texture_pixel(to, 0, 0, 0x00ff00ff);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

/* Change only an associated handle; the texture table still contains 17. */
static void texture_rebind_patch_association(const char *path)
{
    static const uint8_t association[] = {
        0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 17, 2, 0,
    };
    g_autofree char *stream = NULL;
    gsize len, found = 0;
    GError *err = NULL;

    g_assert_true(g_file_get_contents(path, &stream, &len, &err));
    g_assert_no_error(err);
    for (gsize at = 0; at + sizeof(association) <= len; at++) {
        if (!memcmp(stream + at, association, sizeof(association))) {
            stl_be_p(stream + at + 9, 0x12345678);
            found++;
        }
    }
    g_assert_cmpuint(found, ==, 1);
    g_assert_true(g_file_set_contents(path, stream, len, &err));
    g_assert_no_error(err);
}

static void texture_rebind_expect_load_failure(const char *uri)
{
    QTestState *to;
    int64_t deadline;
    bool running;

    to = qtest_init(MACHINE_ARGS " -incoming defer");
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(to,
        "{ 'execute': 'migrate-incoming', "
            "'arguments': { 'uri': %s } }", uri);
    deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    do {
        running = qtest_probe_child(to);
        if (!running) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    if (running) {
        qtest_set_expected_status(to, 0);
        qtest_quit(to);
    } else {
        qtest_wait_qemu(to);
        qtest_quit(to);
    }
    g_assert_false(running);
}

static void test_texture_rebind_migration_rejects_missing_object(void)
{
    QTestState *from;
    g_autofree char *dir = NULL, *stream = NULL, *uri = NULL;
    GError *err = NULL;

    dir = g_dir_make_tmp("sn0-kona-rebind-invalid-XXXXXX", &err);
    g_assert_no_error(err);
    stream = g_build_filename(dir, "migration", NULL);
    uri = g_strdup_printf("file:%s", stream);
    from = qtest_init(MACHINE_ARGS);
    texture_rebind_uploads(from, false);
    texture_rebind_packet(from, 1, 17);
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);

    texture_rebind_patch_association(stream);
    texture_rebind_expect_load_failure(uri);
    g_assert_cmpint(g_unlink(stream), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* Locate one VMState subsection header and return its first data byte. */
static gsize kona_stream_subsection(const char *stream, gsize len,
                                    const char *name, uint32_t version,
                                    unsigned expected)
{
    gsize name_len = strlen(name), payload = 0;
    unsigned found = 0;

    for (gsize at = 0; at + name_len + 6 <= len; at++) {
        if ((uint8_t)stream[at] == QEMU_VM_SUBSECTION &&
            (uint8_t)stream[at + 1] == name_len &&
            !memcmp(stream + at + 2, name, name_len) &&
            ldl_be_p(stream + at + 2 + name_len) == version) {
            payload = at + name_len + 6;
            found++;
        }
    }
    g_assert_cmpuint(found, ==, expected);
    return payload;
}

/* The read-binding marker identifies this context's GL pipe record. */
static void texture_rebind_patch_absent_subsection(const char *path)
{
    static const char dma_name[] = "sgi-kona/dma";
    static const uint8_t pipe[] = {
        1, 0x6d, 0x3b, 0x7a, 0x51, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    g_autofree char *stream = NULL;
    GError *err = NULL;
    gsize len, start, end;
    unsigned found = 0;

    g_assert_true(g_file_get_contents(path, &stream, &len, &err));
    g_assert_no_error(err);
    start = kona_stream_subsection(stream, len, "sgi-kona/contexts", 7, 1);
    end = kona_stream_subsection(stream, len, dma_name, 2, 1) -
          sizeof(dma_name) - 5;
    g_assert_cmpuint(start, <, end);
    kona_stream_subsection(stream, len, "sgi-kona/textures", 2, 0);
    for (gsize at = start; at + sizeof(pipe) <= end; at++) {
        if (!memcmp(stream + at, pipe, sizeof(pipe))) {
            stl_be_p(stream + at + 14, 0x12345678);
            stream[at + 18] = 2;
            found++;
        }
    }
    g_assert_cmpuint(found, ==, 1);
    g_assert_true(g_file_set_contents(path, stream, len, &err));
    g_assert_no_error(err);
}

static void test_texture_rebind_migration_rejects_absent_table(void)
{
    QTestState *from, *to;
    g_autofree char *dir = NULL, *stream = NULL, *uri = NULL;
    GError *err = NULL;

    dir = g_dir_make_tmp("sn0-kona-rebind-absent-XXXXXX", &err);
    g_assert_no_error(err);
    stream = g_build_filename(dir, "migration", NULL);
    uri = g_strdup_printf("file:%s", stream);
    from = qtest_init(MACHINE_ARGS);
    gl_make_current(from);
    kona_command(from, 0x114, (uint32_t[]){ 0x6d3b7a51 }, 1);
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);

    to = qtest_init(MACHINE_ARGS " -incoming defer");
    qtest_qmp_assert_success(to,
        "{ 'execute': 'migrate-incoming', "
            "'arguments': { 'uri': %s } }", uri);
    sgi_wait_migration(to, "completed");
    qtest_quit(to);

    texture_rebind_patch_absent_subsection(stream);
    texture_rebind_expect_load_failure(uri);
    g_assert_cmpint(g_unlink(stream), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_texture_rebind_pending_migration(void)
{
    QTestState *from = qtest_init(MACHINE_ARGS), *to;
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    texture_rebind_uploads(from, false);
    kona_command(from, 0x178, (uint32_t[]) { 1 }, 1);
    kona_command(from, 0x106,
                 (uint32_t[]) { 17, (2u << 28) | (41 << 12),
                                0x30 << 14, 0x11, 1 }, 5);
    dir = g_dir_make_tmp("sn0-kona-rebind-pending-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    kona_command(to, 0x179, record, ARRAY_SIZE(record));
    assert_texture_pixel(to, 0, 0, texture_source[0]);
    texture_rebind_packet(to, 0, 29);
    texture_rebind_packet(to, 1, UINT32_MAX);
    assert_texture_pixel(to, 0, 0, texture_source[0]);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_texture_rebind_split_record(void)
{
    QTestState *from = qtest_init(MACHINE_ARGS), *to;
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    texture_rebind_uploads(from, false);
    kona_command(from, 0x178, (uint32_t[]) { 1 }, 1);
    kona_command(from, 0x106,
                 (uint32_t[]) { 17, (2u << 28) | (41 << 12),
                                0x30 << 14, 0x11, 1 }, 5);
    qtest_writel(from, REG_COMMAND, 0x179);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(from, REG_COMMAND, record[k]);
    }
    dir = g_dir_make_tmp("sn0-kona-rebind-record-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    for (unsigned k = 4; k < ARRAY_SIZE(record); k++) {
        qtest_writel(to, REG_COMMAND, record[k]);
    }
    assert_texture_pixel(to, 0, 0, texture_source[0]);
    texture_rebind_packet(to, 0, 29);
    texture_rebind_packet(to, 1, UINT32_MAX);
    assert_texture_pixel(to, 0, 0, texture_source[0]);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_texture_rebind_split_bind(void)
{
    QTestState *from = qtest_init(MACHINE_ARGS), *to;
    uint32_t bind[5] = { 17, (2u << 28) | (41 << 12),
                         0x30 << 14, 0x11, 1 };
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    texture_rebind_uploads(from, false);
    kona_command(from, 0x178, (uint32_t[]) { 1 }, 1);
    qtest_writel(from, REG_COMMAND, 0x106);
    qtest_writel(from, REG_COMMAND, bind[0]);
    qtest_writel(from, REG_COMMAND, bind[1]);
    dir = g_dir_make_tmp("sn0-kona-rebind-bind-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    for (unsigned k = 2; k < ARRAY_SIZE(bind); k++) {
        qtest_writel(to, REG_COMMAND, bind[k]);
    }
    kona_command(to, 0x179, record, ARRAY_SIZE(record));
    assert_texture_pixel(to, 0, 0, texture_source[0]);
    texture_rebind_packet(to, 0, 29);
    texture_rebind_packet(to, 1, UINT32_MAX);
    assert_texture_pixel(to, 0, 0, texture_source[0]);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_texture_rebind_interrupted(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };

    texture_rebind_uploads(s, false);
    kona_command(s, 0x178, (uint32_t[]) { 1 }, 1);
    kona_command(s, 0x106,
                 (uint32_t[]) { 17, (2u << 28) | (41 << 12),
                                0x30 << 14, 0x11, 1 }, 5);
    kona_command(s, 0x178, (uint32_t[]) { 0 }, 1);
    kona_command(s, 0x179, record, ARRAY_SIZE(record));
    texture_rebind_packet(s, 1, UINT32_MAX);
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

static void texture_constant_upload(QTestState *s, uint32_t handle,
                                      uint32_t source_format, uint32_t pixel)
{
    uint32_t h[20];
    unsigned bytes = source_format == 4 ? 3 : 4;
    unsigned stride = ROUND_UP(2 * bytes, 4);

    memcpy(h, texture_header, sizeof(h));
    h[3] = source_format;
    h[5] = 3;
    h[7] = 1;
    kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
    h[10] = handle;
    kona_command(s, 0x174, h, 20);
    for (unsigned y = 0; y < 2; y++) {
        unsigned phase = (3 + y * stride) & 3;
        unsigned count = DIV_ROUND_UP(phase + 2 * bytes, 4);

        for (unsigned at = 0; at < count; at++) {
            uint32_t word = 0;

            for (unsigned k = 0; k < 4; k++) {
                int offset = at * 4 + k - phase;
                unsigned byte = offset >= 0 && offset < 2 * bytes ?
                    (pixel >> (24 - (offset % bytes) * 8)) & 255 : 0xee;

                word = (word << 8) | byte;
            }
            qtest_writel(s, REG_COMMAND, word);
        }
    }
    kona_command(s, 0x176, (uint32_t[]) { handle }, 1);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 0 }, 2);
}

static void texture_native_format_bind(QTestState *s, unsigned index,
                                       unsigned mag, unsigned channel,
                                       unsigned detail_level)
{
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };
    uint32_t bind[5] = {
        0, (2u << 28) | (index << 12) | 7,
        (0x30 << 14) | (mag << 5) | (channel << 26) | (detail_level << 21),
        0x11, 1,
    };

    kona_command(s, 0x1d5, bind, 5);
    kona_command(s, 0x179, record, 22);
}

static void test_texture_ordinary_source(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const unsigned formats[] = { 0, 1, 2, 3, 6, 7, 9, 10, 8 };
    static const uint32_t expected[] = {
        0x800000ff, 0x008000ff, 0x000080ff, 0x00000080,
        0x808080ff, 0x80808040, 0x204080ff, 0x20408010, 0x10204080,
    };
    unsigned index = mode < 18 ? mode % 9 : 1;
    unsigned format = formats[index];
    unsigned components = format == 7 ? 2 : format == 9 ? 3 :
                          (format == 8 || format == 10) ? 4 : 1;
    bool dma = (mode >= 9 && mode < 18) || mode == 19;
    bool migrate = mode == 18 || mode == 19;
    uint32_t pixel = mode == 20 ? 0 : expected[index];
    unsigned bytes = 4 * components;
    uint8_t source[20] = { 0 };
    uint32_t h[20];
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    texture_native_bind(s, 0);
    memcpy(h, texture_header, sizeof(h));
    h[3] = format;
    h[5] = 1;
    h[6] = dma;
    h[17] |= dma ? 4 : 0;
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    for (unsigned k = 0; k < 4; k++) {
        for (unsigned c = 0; c < components; c++) {
            source[1 + k * components + c] = 128 >> c;
        }
    }
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380001, source + 1, bytes);
    }
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    if (!dma) {
        qtest_writel(s, REG_COMMAND, ldl_be_p(source));
    }
    if (mode == 20) {
        qtest_writel(s, REG_COMMAND + 0xc + 0x20 * 0x1fc, 0);
        return_context_restore(s, SAVE_AREA);
    }
    if (migrate) {
        dir = g_dir_make_tmp("sn0-kona-texture-source-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (dma) {
        texture_dma_packet(s, 1, bytes, bytes, 0);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else if (mode != 14) {
        for (unsigned k = 1; k < DIV_ROUND_UP(1 + bytes, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(source + k * 4));
        }
    }
    assert_texture_pixel(s, 0, 0, 0);
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2, pixel);
    }
    texture_native_format_bind(s, 37, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, mode == 20 ? 0 : pixel | 0xff);
    texture_native_format_bind(s, 41, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, pixel);
    texture_native_format_bind(s, 0, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, 0);
    texture_native_format_bind(s, 4095, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, 0);
    texture_native_format_bind(s, 41, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, pixel);
    assert_textport_in_step(s);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_texture_native_rgb(gconstpointer opaque)
{
    unsigned format = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    texture_native_bind(s, 0);
    texture_constant_upload(s, 0, format, 0x4080c020);
    assert_texture_pixel(s, 0, 0, format == 4 ? 0x4080c0ff : 0x4080c020);
    texture_native_format_bind(s, 37, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, 0x4080c0ff);
    texture_native_format_bind(s, 41, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, format == 4 ? 0x4080c0ff : 0x4080c020);
    /* RGB storage cannot bind a luminance or unknown destination format. */
    texture_native_format_bind(s, 0, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, 0);
    texture_native_format_bind(s, 4095, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, 0);
    texture_native_format_bind(s, 41, 0, 0, 0);
    assert_texture_pixel(s, 0, 0, format == 4 ? 0x4080c0ff : 0x4080c020);
    assert_textport_in_step(s);
    qtest_quit(s);
}

static void test_texture_negative_bound_filters(void)
{
    static const float vertices[3][2] = {
        { -0.5, -0.5 }, { 0.5, -0.5 }, { 0, 0.5 },
    };
    uint32_t bind[5] = { 0, (2u << 28) | (41 << 12),
                         (0x30 << 14) | (1 << 5), 0x11, 1 };
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    texture_native_bind(s, 0);
    kona_command(s, 0x174, texture_header, ARRAY_SIZE(texture_header));
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, texture_source[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    kona_command(s, 0x1d5, bind, ARRAY_SIZE(bind));
    for (unsigned pass = 0; pass < 2; pass++) {
        gl_window_cleared(s, 0, 0, 1);
        record[2] = (pass ? 32 : 256) << 10;
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
        kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
        kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
        gl_begin(s, 4);
        for (unsigned k = 0; k < 3; k++) {
            kona_command_f(s, 0x80,
                (float[]) { 128 * vertices[k][0] + 0.5,
                             128 * vertices[k][1] + 0.5 }, 2);
            kona_command_f(s, 0x8c, vertices[k], 2);
        }
        gl_end(s);
        /* Negative MAX_LOD chooses linear magnification for this footprint. */
        assert_stencil_colour(s, pass ? 0x808080 : 0xffffff);
    }
    qtest_quit(s);
}

static void test_texture_native_detail(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t words[33] = { 0 };
    uint32_t args[] = { mode == 4 ? UINT32_MAX : mode == 5 ? 2 : 1,
                        1, 1, 41, 0, 0 };
    uint32_t expected = mode == 1 ? 0x5060c0 : mode == 3 ? 0x3060d0 :
                        mode == 4 ? 0x4080c0 : mode == 5 ? 0x0000ff :
                        mode == 9 || mode == 11 || mode == 13 ? 0x6060c0 :
                                                              0x8040c0;
    QTestState *s = qtest_init(MACHINE_ARGS), *from = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    texture_native_bind(s, 0);
    texture_constant_upload(s, 0, 5, 0x4080c080);
    texture_constant_upload(s, 1, 5, 0xff0080ff);
    /* Negative bound cases distinguish curve nodes 16 and 32. */
    for (unsigned group = 0; group < 16; group++) {
        uint64_t packed = 0;

        for (unsigned k = 0; k < 4; k++) {
            uint64_t coefficient = mode >= 9 ? (group * 4 + k) * 8 : 256;

            packed |= coefficient << (10 * k);
        }
        words[group * 2] = packed;
        words[group * 2 + 1] = packed >> 32;
    }
    curve_open(s, 7);
    curve_words(s, words, 33);
    kona_command(s, 0x17d, args, 6);
    if (mode == 7) {
        args[3] = 4095; /* Invalid format cannot replace the valid binding. */
        kona_command(s, 0x17d, args, 6);
    }
    texture_native_format_bind(s, 41, mode == 1 ? 4 : 3,
                               mode == 2 ? 1 : mode == 3 ? 2 : 0,
                               mode == 8 ? 4 : 0);
    if (mode >= 9) {
        uint32_t record[22] = { 0x43, 0x101010, 0 };
        unsigned bound = mode == 10 || mode == 12 ? 32 : 16;

        record[2] = mode == 11 || mode == 12 ? bound << 10 :
                                                    bound | (256 << 10);
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
    }
    if (mode == 6 || mode == 13) {
        dir = g_dir_make_tmp("sn0-kona-detail-XXXXXX", &error);
        g_assert_no_error(error);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        from = s;
        s = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
    }
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    if (mode == 3) {
        kona_command(s, 0x24, (uint32_t[]) { 0xbe2 }, 1);
        kona_command(s, 0xcb, (uint32_t[]) { 0x302, 0x303 }, 2);
    }
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    if (mode == 11 || mode == 12) {
        gl_begin(s, 4);
        for (unsigned k = 0; k < 3; k++) {
            kona_command_f(s, 0x80,
                           (float[]) { 100 * tri[k][0], 100 * tri[k][1] }, 2);
            kona_command_f(s, 0x90,
                           (float[]) { tri[k][0], tri[k][1], tri[k][2] }, 3);
        }
        gl_end(s);
    } else {
        gl_triangle_z(s, tri, 0);
    }
    assert_stencil_colour(s, expected);
    assert_textport_in_step(s);
    qtest_quit(s);
    if (from) {
        qtest_quit(from);
        g_rmdir(dir);
    }
}

static void texture_volume_bind(QTestState *s, bool linear, bool mipmapped)
{
    uint32_t bind[] = {
        0, (3u << 28) | (41 << 12),
        (0x30 << 14) | (mipmapped ? 3 << 2 : linear ? (1 << 2) | (1 << 5) : 0),
        0x111, 1,
    };
    uint32_t mip[] = { 0x22, 0x101010, 256u << 10, 0x113, 0x55, 0x66,
                       UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                       UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };

    kona_command(s, 0x1d5, bind, 5);
    if (mipmapped) {
        kona_command(s, 0x17b, mip, ARRAY_SIZE(mip));
    } else {
        kona_command(s, 0x17c, (uint32_t[]) { 0x22, 0x113, 0x55, 0x66 }, 4);
    }
}

static void texture_volume_header(QTestState *s, unsigned z, bool dma,
                                   bool odd)
{
    uint32_t h[20];

    memcpy(h, texture_header, sizeof(h));
    h[0] = 0x806f;
    h[1] = h[2] = odd ? 1 : 2;
    h[5] = dma ? 3 : 0;
    h[6] = dma;
    h[7] = 0;
    h[13] = h[14] = odd ? 0 : 1;
    h[15] = z;
    h[16] = 0x113;
    h[17] = 0x1141 | (dma ? 4 : 0) | (odd ? 8 : 0);
    h[18] = 0x55;
    h[19] = 0x66;
    kona_command(s, 0x174, h, 20);
}

static void test_texture_native_volume(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool linear = mode == 1, dma = mode == 2, missing = mode == 4;
    bool mipmapped = mode == 5 || mode == 6, mismatch = mode == 9;
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = missing || mismatch ? kona_init_logged(&log) :
                                       qtest_init(MACHINE_ARGS);
    QTestState *from = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    texture_volume_bind(s, linear, mipmapped);
    for (unsigned z = 0; z < (missing ? 1 : 2); z++) {
        uint32_t pixel = z ? 0x0000ffff : 0xff0000ff;

        texture_volume_header(s, z, dma, false);
        if (dma) {
            uint8_t bytes[16];

            for (unsigned k = 0; k < 4; k++) {
                stl_be_p(bytes + k * 4, pixel);
            }
            qtest_memwrite(s, 0x380003, bytes, sizeof(bytes));
            qtest_writel(s, KONA_REG(0x100000), 0x20000380);
            texture_dma_packet(s, 3, sizeof(bytes), sizeof(bytes), 0);
            g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
        } else {
            for (unsigned k = 0; k < 4; k++) {
                qtest_writel(s, REG_COMMAND, pixel);
                if (mode == 3 && z == 1 && k == 1) {
                    dir = g_dir_make_tmp("sn0-kona-volume-XXXXXX", &error);
                    g_assert_no_error(error);
                    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
                    from = s;
                    s = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
                    sgi_migrate_stopped(from, uri);
                }
            }
        }
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    }
    if (mipmapped) {
        texture_volume_header(s, 0, false, true);
        qtest_writel(s, REG_COMMAND, 0x00ff00ff);
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
        if (mode == 6) {
            dir = g_dir_make_tmp("sn0-kona-volume-XXXXXX", &error);
            g_assert_no_error(error);
            uri = g_strdup_printf("unix:%s/migrate.sock", dir);
            from = s;
            s = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
            sgi_migrate_stopped(from, uri);
        }
    }
    if (mismatch) {
        kona_command(s, 0x17c,
                     (uint32_t[]) { 0x22, 0x114, 0x55, 0x66 }, 4);
    }
    kona_command(s, 0x24, (uint32_t[]) { 0x806f }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    if (mode == 7 || mode == 8) {
        bool enabled = mode == 7;

        kona_command(s, enabled ? 0x24 : 0x25,
                     (uint32_t[]) { 0x806f }, 1);
        kona_command(s, 0xc9, (uint32_t[]) { 0x2000 }, 1);
        assert_reply_posted(s);
        kona_command(s, enabled ? 0x25 : 0x24,
                     (uint32_t[]) { 0x806f }, 1);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
        kona_command_f(s, 0x84, (float[]) { 0.25, 0.25, 0.25 }, 3);
        gl_triangle_z(s, tri, 0);
        assert_stencil_colour(s, enabled ? 0xff0000 : 0xffffff);
    } else if (mipmapped) {
        /* Only R varies; its native derivative must select the green mip. */
        gl_begin(s, 4);
        for (unsigned k = 0; k < 3; k++) {
            kona_command_f(s, 0x84,
                           (float[]) { 0.25, 0.25, k == 1 ? 128 : 0 }, 3);
            kona_command_f(s, 0x90,
                           (float[]) { tri[k][0], tri[k][1], 0 }, 3);
        }
        gl_end(s);
        assert_stencil_colour(s, 0x00ff00);
    } else {
        kona_command_f(s, 0x84, (float[]) { 0.25, 0.25, 0.25 }, 3);
        gl_triangle_z(s, tri, 0);
        assert_stencil_colour(s, missing || mismatch ? 0xffffff : 0xff0000);
        if (!missing && !mismatch) {
            kona_command_f(s, 0x84, (float[]) { 0.25, 0.25, 0.75 }, 3);
            gl_triangle_z(s, tri, 0);
            assert_stencil_colour(s, 0x0000ff);
            kona_command_f(s, 0x84, (float[]) { 0.25, 0.25, 0.5 }, 3);
            gl_triangle_z(s, tri, 0);
            assert_stencil_colour(s, linear ? 0x800080 : 0x0000ff);
            kona_command_f(s, 0x84, (float[]) { 0.25, 0.25, 1.25 }, 3);
            gl_triangle_z(s, tri, 0);
            assert_stencil_colour(s, 0xff0000);
        }
    }
    assert_textport_in_step(s);
    qtest_quit(s);
    if (missing || mismatch) {
        found = kona_log_read(log);
        g_assert_nonnull(strstr(found, "incomplete texture image pyramid, "
                                      "texturing disabled for draw"));
    }
    if (from) {
        qtest_quit(from);
        g_rmdir(dir);
    }
}

static void test_texture_native_integer(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque), type = mode & 7;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    bool dma = mode & 8, migrate = mode & 16, swap = mode & 32;
    bool migrate_state = mode & 64;
    bool sign = !(type & 1);
    uint32_t maximum = sign ? (size == 1 ? 0x7f :
                               size == 2 ? 0x7fff : 0x7fffffff) :
                              (size == 1 ? 0xff :
                               size == 2 ? 0xffff : 0xffffffff);
    uint32_t components[] = {
        sign ? maximum + 1 : 0,
        sign ? maximum / 2 : maximum / 2 + 1, maximum, maximum,
    };
    uint32_t expected = sign ? 0x007fffff : 0x0080ffff;
    uint8_t source[96];
    uint32_t h[20], row_bytes = 8 * size, stride = 12 * size;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    QTestState *s = qtest_init(MACHINE_ARGS), *from = NULL;

    memset(source, 0xee, sizeof(source));
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 2; x++) {
            for (unsigned c = 0; c < 4; c++) {
                for (unsigned k = 0; k < size; k++) {
                    source[y * stride + (x * 4 + c) * size + k] =
                        components[c] >> (8 * (swap ? k : size - 1 - k));
                }
            }
        }
    }
    gl_make_current(s);
    texture_native_bind(s, 0);
    memcpy(h, texture_header, sizeof(h));
    h[4] = type;
    h[5] = 3;
    h[6] = dma;
    h[7] = 1;
    h[17] |= dma ? 4 : 0;
    kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf0, swap }, 2);
    if (migrate_state) {
        dir = g_dir_make_tmp("sn0-kona-integer-XXXXXX", &error);
        g_assert_no_error(error);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        from = s;
        s = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
    }
    kona_command(s, 0x174, h, 20);
    if (dma) {
        /* Byte order belongs to the upload, independent of later pipe state. */
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 0 }, 2);
        qtest_memwrite(s, 0x380003, source, stride + row_bytes);
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        texture_dma_packet(s, 3, row_bytes * 2, row_bytes, stride - row_bytes);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else {
        unsigned sent = 0;

        for (unsigned y = 0; y < 2; y++) {
            unsigned phase = (3 + y * stride) & 3;
            unsigned count = DIV_ROUND_UP(phase + row_bytes, 4);

            for (unsigned at = 0; at < count; at++, sent++) {
                uint32_t word = 0;

                for (unsigned k = 0; k < 4; k++) {
                    int offset = at * 4 + k - phase;
                    unsigned byte = offset >= 0 && offset < row_bytes ?
                                    source[y * stride + offset] : 0xee;

                    word = (word << 8) | byte;
                }
                qtest_writel(s, REG_COMMAND, word);
                if (migrate && sent == 1) {
                    dir = g_dir_make_tmp("sn0-kona-integer-XXXXXX", &error);
                    g_assert_no_error(error);
                    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
                    from = s;
                    s = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
                    sgi_migrate_stopped(from, uri);
                }
            }
        }
    }
    assert_texture_pixel(s, 0, 0, 0);
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(s, 0, 0, expected);
    assert_texture_pixel(s, 1, 1, expected);
    assert_textport_in_step(s);
    qtest_quit(s);
    if (from) {
        qtest_quit(from);
        g_rmdir(dir);
    }
}

static void test_texture_native_float(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode == 1 || mode == 4, swap = mode >= 3;
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
    h[7] = !dma;
    if (!dma) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
    }
    h[17] |= dma ? 4 : 0;
    if (swap) {
        for (unsigned k = 0; k < ARRAY_SIZE(source); k++) {
            source[k] = bswap32(source[k]);
        }
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
    }
    kona_command(s, 0x174, h, 20);
    if (dma) {
        uint8_t bytes[sizeof(source)];

        for (unsigned k = 0; k < ARRAY_SIZE(source); k++) {
            stl_be_p(bytes + k * 4, source[k]);
        }
        qtest_memwrite(s, 0x380003, bytes, sizeof(bytes));
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        texture_dma_packet(s, 3, sizeof(bytes), sizeof(bytes), 0);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else {
        texture_float_rows(s, source, 3, 0, 18);
    }
    assert_texture_pixel(s, 0, 0, 0);
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(s, 0, 0, 0x4080bfff);
    assert_texture_pixel(s, 1, 1, 0xff0000ff);
    if (mode == 2 || mode == 5) {
        /* Refusing a nonfinite last component preserves the published image. */
        source[15] = swap ? 0x0000c07f : 0x7fc00000;
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
    h[6] = dma;
    h[7] = !dma;
    if (!dma) {
        kona_command(from, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
    }
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
        texture_dma_packet(from, 3, sizeof(source), sizeof(source), 0);
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
        texture_dma_packet(to, 3, sizeof(source), sizeof(source), 0);
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
    uint32_t record[22] = { 0x44, 0x101010, 256u << 10 };

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    memcpy(h, texture_header, sizeof(h));
    h[3] = cases[c].source;
    h[9] = cases[c].storage;
    h[17] = 0x2200;
    h[7] = 1;
    kona_command(s, 0x1d5,
                 (uint32_t[]) { 0, (2u << 28) |
                     ((h[9] == 0xe ? 17 : h[9] == 8 ? 1 : 0) << 12),
                     h[9] << 14, 0x11, 1 }, 5);
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

static void test_texture_native_family(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t h[20];
    uint32_t record[22] = { 0x44, 0x101010, 256u << 10 };
    uint32_t bind[] = { 0, (2u << 28) | (3 << 12), 0xd << 14, 0x11, 1 };
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command(s, 0x1d5, bind, ARRAY_SIZE(bind));
    kona_command(s, 0x179, record, ARRAY_SIZE(record));
    memcpy(h, texture_header, sizeof(h));
    h[3] = 6;
    h[4] = 3;
    h[9] = 0xd;
    h[17] = 0x2200;
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    qtest_writel(s, REG_COMMAND, 0x40004000);
    qtest_writel(s, REG_COMMAND, 0x40004000);
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(s, 0, 0, 0x400000ff);
    if (mode == 1) {
        bind[1] = (2u << 28) | (63 << 12);
    } else if (mode == 2) {
        bind[4] = 0;
    } else if (mode == 3) {
        bind[4] = 2;
    } else if (mode == 4) {
        record[0] = 0x43;
    } else if (mode == 5) {
        h[17] = 0x2180;
        kona_command(s, 0x174, h, ARRAY_SIZE(h));
        qtest_writel(s, REG_COMMAND, 0xffff0000);
        qtest_writel(s, REG_COMMAND, 0x0000ffff);
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    }
    kona_command(s, 0x1d5, bind, ARRAY_SIZE(bind));
    kona_command(s, 0x179, record, ARRAY_SIZE(record));
    assert_texture_pixel(s, 0, 0, mode >= 1 && mode <= 4 ? 0 : 0x400000ff);
    if (mode >= 1 && mode <= 4) {
        bind[1] = (2u << 28) | (3 << 12);
        bind[4] = 1;
        record[0] = 0x44;
        kona_command(s, 0x1d5, bind, ARRAY_SIZE(bind));
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
        assert_texture_pixel(s, 0, 0, 0x400000ff);
    }
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.25 }, 2);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, 0x404040);
    assert_textport_in_step(s);
    qtest_quit(s);
}

static void test_texture_native_rows(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t h[20];
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10, 0x44, 0, 0, 0x55 };

    gl_make_current(s);
    memcpy(h, texture_header, sizeof(h));
    h[1] = h[2] = 16;
    h[13] = h[14] = 15;
    h[16] = 0x44;
    h[18] = 0x55;
    kona_command(s, 0x1d5,
                 (uint32_t[]) { 0, (2u << 28) | (41 << 12),
                                0x30 << 14, 0x44, 1 }, 5);
    /* Select the physical descriptor supplied by this upload. */
    kona_command(s, 0x179, record, 22);
    h[7] = 1;
    kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 17 }, 2);
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
    uint32_t record[22] = { 0x44, 0x101010, 256u << 10 };

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
    h[17] = 0x2200;
    h[7] = 1;
    kona_command(from, 0x1d5,
                 (uint32_t[]) { 0, 2u << 28, 0xd << 14, 0x11, 1 }, 5);
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

static void draw_pixels_expected(QTestState *s, bool drawn)
{
    g_autofree Expected *e = g_new(Expected, 1);
    static const uint32_t rgb[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };

    expected_clear(e, 0x000000);
    if (drawn) {
        for (unsigned k = 0; k < 4; k++) {
            unsigned at = ((127 - (64 + k / 2)) * 128 + 64 + k % 2) * 3;

            e->rgb[at] = rgb[k] >> 16;
            e->rgb[at + 1] = rgb[k] >> 8;
            e->rgb[at + 2] = rgb[k];
        }
    }
    assert_window_matches(s, e);
}

static void test_draw_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode == 2 || mode == 3 || mode == 12 || mode == 17 || mode == 19;
    bool padded = mode == 1 || mode == 3;
    bool migrate = mode == 4 || mode == 12 || mode == 13 || mode == 24;
    bool refused = mode >= 5 && mode < 18 && mode != 12 &&
                   mode != 15 && mode != 16;
    uint32_t h[] = { 2, 2, 5, 1, padded ? 3 : 0, dma, padded, 0 };
    static const uint32_t plain[] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    static const uint32_t pad[] = {
        0xaaaaaaff, 0x0000ff00, 0xff00ffaa,
        0xaaaaaa00, 0x00ffffff, 0xffffffaa,
    };
    static const uint32_t contiguous[] = {
        0xaaff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xffaaaaaa,
    };
    const uint32_t *data = (mode == 18 || mode == 24) ? contiguous :
                           padded ? pad : plain;
    unsigned words = (mode == 18 || mode == 24) ? ARRAY_SIZE(contiguous) :
                     padded ? ARRAY_SIZE(pad) : ARRAY_SIZE(plain);
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *tmpfs = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (padded) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
    }
    if (mode == 5 || mode == 13) {
        kona_command(s, 0x2f, (uint32_t[]) { 0x7f800000, 0x3f800000 }, 2);
    } else if (mode == 16) {
        kona_command_f(s, 0x2f, (float[]) { 2, 1 }, 2);
    } else if (mode == 6) {
        kona_command(s, 0x30, (uint32_t[]) { 0x0d10, 0x3f800000 }, 2);
    } else if (mode == 7) {
        kona_command(s, 0x24, (uint32_t[]) { 0x8011 }, 1);
    } else if (mode == 8) {
        kona_command(s, 0xd0, (uint32_t[]) { 0x1800 }, 1);
        kona_command(s, 0xd2, (uint32_t[]) { 0 }, 1);
    } else if (mode == 9) {
        gl_begin(s, 4);
    } else if (mode == 10) {
        h[7] = 1;
    } else if (mode == 11) {
        h[2] = 11;
        h[3] = 2;
    }
    if (mode >= 20 && mode <= 23) {
        uint32_t malformed[] = { 2, 2, 5, 1, 0, 0, 0, 0 };

        if (mode == 20) {
            malformed[0] = UINT32_MAX;
        } else if (mode == 21) {
            malformed[2] = UINT32_MAX;
        } else if (mode == 22) {
            malformed[4] = 4;
        } else {
            malformed[6] = 2;
        }
        kona_command(s, 0x3a, malformed, ARRAY_SIZE(malformed));
    }
    if (mode == 18 || mode == 24) {
        h[4] = 1;
    } else if (mode == 19) {
        h[6] = 1;
        kona_command(s, 0x33, (uint32_t[]) { 0x0cf2, 4 }, 2);
    }
    if (mode == 14) {
        return_context_deactivate(s);
    } else if (mode == 15) {
        kona_command(s, 0x24, (uint32_t[]) { 0xde0 }, 1);
    } else if (mode == 16) {
        kona_command_f(s, 0x2f, (float[]) { 1, 1 }, 2);
    }
    kona_command(s, 0x3a, h, ARRAY_SIZE(h));
    if (!dma) {
        qtest_writel(s, REG_COMMAND, data[0]);
    }
    if (mode == 17) {
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    }
    if (migrate) {
        tmpfs = g_dir_make_tmp("sn0-kona-draw-pixels-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (dma) {
        static const uint8_t bytes[] = {
            0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
            0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        };

        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        unsigned stride = mode == 19 ? 16 : padded ? 12 : 8;

        qtest_memwrite(s, 0x380000 + h[4], bytes, 8);
        qtest_memwrite(s, 0x380000 + h[4] + stride, bytes + 8, 8);
        texture_dma_packet(s, h[4], sizeof(bytes), h[6] ? 8 : 16,
                           h[6] ? stride - 8 : 0);
    } else {
        for (unsigned k = 1; k < words; k++) {
            qtest_writel(s, REG_COMMAND, data[k]);
        }
    }
    if (mode == 9) {
        gl_end(s);
    }
    kona_command_f(s, 0x2f, (float[]) { 1, 1 }, 2);
    draw_pixels_expected(s, !refused);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (tmpfs) {
        g_rmdir(tmpfs);
    }
}

static void test_draw_pixels_raw_pio(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t h[] = { 2, 2, 5, 1, 0, 0, 0, 0 };
    uint32_t data[4] = { 0xff0000ff, 0x00ff00ff,
                         0x0000ffff, 0xffffffff };
    uint32_t rgb[4] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
    unsigned words = 4;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    g_autofree char *tmpfs = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (mode == 0) {
        h[0] = 1;
        h[2] = 4;
        data[0] = 0x4080c0aa;
        data[1] = 0x102030bb;
        rgb[0] = 0x4080c0;
        rgb[1] = 0x102030;
        words = 2;
    } else if (mode == 1) {
        h[2] = 6;
        h[4] = 1;
        data[0] = 0xaa4080bb;
        data[1] = 0xccc0ffdd;
        rgb[0] = 0x404040;
        rgb[1] = 0x808080;
        rgb[2] = 0xc0c0c0;
        rgb[3] = 0xffffff;
        words = 2;
    } else if (mode == 2) {
        h[2] = 7;
        h[4] = 3;
        h[6] = 1;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
        data[0] = 0xaaaaaa40;
        data[1] = 0x8080ffbb;
        data[2] = 0xaaaaaac0;
        data[3] = 0x40ffffbb;
        rgb[0] = 0x404040;
        rgb[1] = 0x808080;
        rgb[2] = 0xc0c0c0;
        rgb[3] = 0xffffff;
    } else if (mode == 3) {
        h[2] = 3;
        kona_command(s, 0x30, (uint32_t[]) { 0xd15, 0x3f800000 }, 2);
        kona_command(s, 0x30, (uint32_t[]) { 0xd19, 0x3f000000 }, 2);
        kona_command(s, 0x30, (uint32_t[]) { 0xd1b, 0x3e800000 }, 2);
        data[0] = 0x4080aabb;
        data[1] = 0xc0ffccdd;
        for (unsigned k = 0; k < 4; k++) {
            rgb[k] = 0xff8040;
        }
        words = 2;
    } else {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 1 }, 2);
        h[6] = 1;
    }
    kona_command(s, 0x3a, h, ARRAY_SIZE(h));
    qtest_writel(s, REG_COMMAND, data[0]);
    if (mode == 5) {
        tmpfs = g_dir_make_tmp("sn0-kona-raw-pixels-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    for (unsigned k = 1; k < words; k++) {
        qtest_writel(s, REG_COMMAND, data[k]);
    }
    expected_clear(e, 0);
    for (unsigned k = 0; k < h[0] * h[1]; k++) {
        unsigned x = 64 + k % h[0], y = 64 + k / h[0];
        unsigned at = ((127 - y) * 128 + x) * 3;

        e->rgb[at] = rgb[k] >> 16;
        e->rgb[at + 1] = rgb[k] >> 8;
        e->rgb[at + 2] = rgb[k];
    }
    assert_window_matches(s, e);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (tmpfs) {
        g_rmdir(tmpfs);
    }
}

static void draw_pixels_zoom_expected(QTestState *s, float zx, float zy)
{
    g_autofree Expected *e = g_new(Expected, 1);
    static const uint32_t rgb[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };

    expected_clear(e, 0x000000);
    if (isfinite(zx) && isfinite(zy)) {
        for (unsigned k = 0; k < 4; k++) {
            double x0 = 64 + (k % 2) * zx, x1 = x0 + zx;
            double y0 = 64 + (k / 2) * zy, y1 = y0 + zy;

            for (unsigned y = 0; y < 128; y++) {
                for (unsigned x = 0; x < 128; x++) {
                    unsigned at = ((127 - y) * 128 + x) * 3;

                    if (x + 0.5 >= MIN(x0, x1) &&
                        x + 0.5 < MAX(x0, x1) &&
                        y + 0.5 >= MIN(y0, y1) &&
                        y + 0.5 < MAX(y0, y1)) {
                        e->rgb[at] = rgb[k] >> 16;
                        e->rgb[at + 1] = rgb[k] >> 8;
                        e->rgb[at + 2] = rgb[k];
                    }
                }
            }
        }
    }
    assert_window_matches(s, e);
}

static void test_draw_pixels_zoom(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode == 1 || mode == 3;
    bool migrate = mode == 7 || mode == 10;
    float zx = 2, zy = 3;
    const uint32_t data[] = { 0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff };
    uint32_t h[] = { 2, 2, 5, 1, 0, dma, 0, 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *tmpfs = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (mode == 2 || mode == 3) {
        zx = -2;
        zy = -1;
    } else if (mode == 4) {
        zx = zy = 0.5;
    } else if (mode == 5) {
        zx = 0;
    } else if (mode == 6) {
        zx = 80;
        zy = 60;
    } else if (mode == 13) {
        zx = INFINITY;
    } else if (mode == 14) {
        zx = 1.5;
        zy = 0.75;
    }
    kona_command_f(s, 0x2f, (float[]) { zx, zy }, 2);
    if (mode == 8 || mode == 9 || mode == 10) {
        kona_command(s, 0xc9, (uint32_t[]) { mode == 9 ? 0x2020 : 0x20 }, 1);
        kona_command_f(s, 0x2f, (float[]) { -1, -1 }, 2);
    } else if (mode == 11) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        gl_window_cleared(s, 0, 0, 0);
        kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
        kona_command_f(s, 0x2f, (float[]) { -1, -1 }, 2);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    } else if (mode == 12) {
        gl_begin(s, 4);
        kona_command_f(s, 0x2f, (float[]) { -1, -1 }, 2);
        gl_end(s);
    } else if (mode == 14) {
        kona_command(s, 0x2f, (uint32_t[]) { 0x7fc00000, 0x3f800000 }, 2);
        kona_command_f(s, 0x2f, (float[]) { zx, zy }, 2);
    }
    if (mode == 15 || mode == 16) {
        if (mode == 15) {
            return_context_deactivate(s);
            attrib_context_fresh(s, SAVE_AREA + 0x20000);
        } else {
            qtest_system_reset(s);
            gl_make_current(s);
        }
        gl_window_cleared(s, 0, 0, 0);
        kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
        zx = zy = 1;
    }
    if (migrate) {
        tmpfs = g_dir_make_tmp("sn0-kona-zoom-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (mode == 8 || mode == 9 || mode == 10) {
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
    }
    kona_command(s, 0x3a, h, ARRAY_SIZE(h));
    if (dma) {
        static const uint8_t bytes[] = {
            0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
            0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        };

        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, bytes, sizeof(bytes));
        texture_dma_packet(s, 0, sizeof(bytes), 16, 0);
    } else {
        for (unsigned k = 0; k < ARRAY_SIZE(data); k++) {
            qtest_writel(s, REG_COMMAND, data[k]);
        }
    }
    draw_pixels_zoom_expected(s, zx, zy);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (tmpfs) {
        g_rmdir(tmpfs);
    }
}

static void pixel_transfer_set(QTestState *s, uint32_t pname, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    kona_command(s, 0x30, (uint32_t[]) { pname, bits }, 2);
}

static void test_draw_pixels_native_dma(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const uint32_t maxima[] = {
        0x7f, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0xffffffff, 0x3f800000,
    };
    unsigned format = mode < 35 ? 3 + mode / 7 : 5;
    unsigned type = mode < 35 ? mode % 7 : 1;
    unsigned components = format == 3 || format == 6 ? 1 :
                          format == 7 ? 2 : format == 4 ? 3 : 4;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    uint32_t h[] = { 2, 1, format, type, 1, 1, 0, 0 };
    uint32_t total = 2 * components * size, line = total, gap = 0;
    uint64_t io = 1;
    bool refused = mode == 40 || mode == 41 || mode == 42 || mode == 44;
    bool migrate = mode == 36 || mode == 43;
    g_autofree uint8_t *bytes = g_malloc0(20000);
    g_autofree Expected *e = g_new(Expected, 1);
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0xca, (uint32_t[]) { 0x204, fbits(0.5) }, 2);
    kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
    if (format == 3) {
        pixel_transfer_set(s, 0xd15, 1);
        pixel_transfer_set(s, 0xd19, 1);
        pixel_transfer_set(s, 0xd1b, 1);
    }
    expected_clear(e, 0);
    for (unsigned c = 0; c < components; c++) {
        uint32_t lo = !(type & 1) && type != 6 ? 1u << (size * 8 - 1) : 0;
        uint32_t hi = maxima[type];

        for (unsigned k = 0; k < size; k++) {
            bytes[c * size + k] = lo >> (8 * (size - 1 - k));
            bytes[(components + c) * size + k] =
                hi >> (8 * (size - 1 - k));
        }
    }
    for (unsigned c = 0; c < 3; c++) {
        e->rgb[((127 - 64) * 128 + 65) * 3 + c] = 255;
    }
    if (mode >= 35) {
        kona_command(s, 0x25, (uint32_t[]) { 0xbc0 }, 1);
        expected_clear(e, 0);
        h[0] = 1;
        h[4] = 0;
        io = 0;
        if (mode == 35 || mode == 36) {
            h[3] = mode == 35 ? 3 : 6;
            total = line = mode == 35 ? 8 : 16;
            kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
            for (unsigned c = 0; c < 4; c++) {
                if (mode == 35) {
                    stw_le_p(bytes + c * 2, (c + 1) * 0x4000 - (c == 3));
                } else {
                    stl_le_p(bytes + c * 4, fbits((c + 1) / 4.0));
                }
            }
            e->rgb[((127 - 64) * 128 + 64) * 3] = 64;
            e->rgb[((127 - 64) * 128 + 64) * 3 + 1] = 128;
            e->rgb[((127 - 64) * 128 + 64) * 3 + 2] = 191;
        } else if (mode == 37) {
            h[2] = 6;
            h[3] = 5;
            total = line = 4;
            stl_be_p(bytes, 0x7fffffff);
            pixel_transfer_set(s, 0xd14, -4294967296.0);
            pixel_transfer_set(s, 0xd15, 2147483648.0);
            e->rgb[((127 - 64) * 128 + 64) * 3] = 128;
            e->rgb[((127 - 64) * 128 + 64) * 3 + 1] = 127;
            e->rgb[((127 - 64) * 128 + 64) * 3 + 2] = 127;
        } else if (mode == 38 || mode == 39 || mode == 43) {
            h[1] = 3;
            h[2] = 4;
            h[4] = mode == 39 ? 1 : 2;
            io = 2;
            if (mode == 39) {
                h[6] = 1;
                total = 9;
                line = 3;
                gap = 3;
                kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 2 }, 2);
                kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
                kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 1 }, 2);
            } else {
                total = line = 11;
                kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 4 }, 2);
            }
            memset(bytes, 0xaa, 15);
            for (unsigned row = 0; row < 3; row++) {
                unsigned at = row * (mode == 39 ? 6 : 4);

                memset(bytes + at, 0, 3);
                bytes[at + row] = 255;
                e->rgb[((127 - (64 + row)) * 128 + 64) * 3 + row] = 255;
            }
        } else if (mode == 40) {
            h[0] = 64;
            h[1] = 65;
            total = line = 16640;
            memset(bytes, 255, total);
        } else if (mode == 41 || mode == 42) {
            total = line = mode == 41 ? 4 : 8;
            h[0] = 2;
        } else if (mode == 44) {
            h[0] = 2;
            h[3] = 6;
            total = line = 32;
            for (unsigned c = 0; c < 8; c++) {
                stl_be_p(bytes + c * 4, c == 7 ? 0x7fc00000 : fbits(1));
            }
        } else if (mode == 45) {
            const float source[] = { 1.5, -0.5, 0.5, 1 };
            const uint32_t scales[] = { 0xd14, 0xd18, 0xd1a, 0xd1c };
            const uint32_t biases[] = { 0xd15, 0xd19, 0xd1b, 0xd1d };

            h[3] = 6;
            total = line = 16;
            for (unsigned c = 0; c < 4; c++) {
                stl_be_p(bytes + c * 4, fbits(source[c]));
                pixel_transfer_set(s, scales[c], 0.5);
                pixel_transfer_set(s, biases[c], 0.25);
            }
            e->rgb[((127 - 64) * 128 + 64) * 3] = 255;
            e->rgb[((127 - 64) * 128 + 64) * 3 + 2] = 128;
        } else if (mode == 46) {
            h[0] = 2;
            total = line = 8;
            stl_be_p(bytes, 0xff0000ff);
            stl_be_p(bytes + 4, 0x00ff00ff);
            kona_command_f(s, 0x2f, (float[]) { -1, 1 }, 2);
            e->rgb[((127 - 64) * 128 + 63) * 3] = 255;
            e->rgb[((127 - 64) * 128 + 62) * 3 + 1] = 255;
        }
    }
    qtest_writel(s, KONA_REG(0x100000), 0x20000380);
    qtest_memwrite(s, 0x380000 + io, bytes,
                  mode == 39 ? 15 : total);
    kona_command(s, 0x3a, h, ARRAY_SIZE(h));
    if (mode == 42) {
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    }
    if (migrate) {
        dir = g_dir_make_tmp("sn0-kona-pixels-dma-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (mode == 47) {
        uint32_t texture[20];

        texture_native_bind(s, 0);
        memcpy(texture, texture_header, sizeof(texture));
        texture[6] = 1;
        texture[7] = 0;
        texture[17] |= 4;
        kona_command(s, 0x174, texture, ARRAY_SIZE(texture));
        total = line = 16;
        for (unsigned k = 0; k < total; k += 4) {
            stl_be_p(bytes + k, 0xff0000ff);
        }
        qtest_memwrite(s, 0x380000, bytes, total);
    }
    if (mode == 0 || mode == 41) {
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX,
                        ==, 0);
    }
    texture_dma_packet(s, io, total, line, gap);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, refused ? 0 : 0x100);
    if (mode == 0 || mode == 41) {
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX,
                        ==, refused ? 0 : INT_CAUSE_MAILBOX);
    }
    assert_window_matches(s, e);
    if (mode == 47) {
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
        assert_texture_pixel(s, 0, 0, 0xff0000ff);
    }
    assert_native_query_word(s, 0xb92, 0x207, false);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_native_raster_texture(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool scalar = mode == 1 || mode == 3 || mode == 8 || mode == 13;
    bool dma = mode == 2 || mode == 3 || mode == 14 || mode == 15;
    bool bitmap = mode == 4 || mode == 5 || mode == 6 || mode == 17;
    uint32_t h[] = { 1, 1, 5, scalar ? 6 : 1, 0, dma, 0, 0 };
    uint32_t source[] = { fbits(1), fbits(1), fbits(1), fbits(1) };
    uint32_t rgb = mode == 9 || mode == 11 || mode == 15 ? 0 :
                   mode == 8 ? 0x800060 : mode == 16 ? 0xffffff : 0x00ff00;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    expected_clear(e, 0);
    if (mode == 7) {
        kona_command_f(s, 0xa9, (float[]) { -1, -1 }, 2);
        kona_command(s, 0x3a, (uint32_t[]) { 1, 1, 5, 1, 0, 0, 0, 0 }, 8);
        qtest_writel(s, REG_COMMAND, 0xffffffff);
        memset(e->rgb + (127 * 128) * 3, 255, 3);
    }
    if (mode != 16) {
        texture_native_bind(s, 0);
        if (mode == 8 || mode == 9) {
            texture_constant_upload(s, 0, 5,
                                    mode == 8 ? 0x8040c080 : 0xffffff40);
        } else {
            kona_command(s, 0x174, texture_header, ARRAY_SIZE(texture_header));
            for (unsigned k = 0; k < 4; k++) {
                qtest_writel(s, REG_COMMAND, texture_source[k]);
            }
            kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
        }
    }
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    if (mode == 10 || mode == 11) {
        kona_command_f(s, 0x88, (float[]) { 1.5, 0.5, 0, mode == 11 ? 0 : 2 },
                       4);
    } else {
        kona_command_f(s, 0x80, (float[]) { 0.75, 0.25 }, 2);
    }
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command_f(s, 0x80, (float[]) { 0.25, 0.75 }, 2);
    if (mode == 8) {
        const uint32_t scales[] = { 0xd14, 0xd18, 0xd1a };
        const uint32_t biases[] = { 0xd15, 0xd19, 0xd1b };

        source[0] = fbits(1.5);
        source[1] = fbits(-0.5);
        source[2] = fbits(0.5);
        for (unsigned c = 0; c < 3; c++) {
            pixel_transfer_set(s, scales[c], 0.5);
            pixel_transfer_set(s, biases[c], 0.25);
        }
    } else if (mode == 9) {
        kona_command(s, 0xca, (uint32_t[]) { 0x204, fbits(0.5) }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
    } else if (mode == 17) {
        pixel_transfer_set(s, 0xd18, 0);
    }
    if (bitmap) {
        if (mode == 6) {
            kona_command(s, 0x5, (uint32_t[]) { 8, 1, 0, 0, 0, 0, 2, 1 }, 8);
            qtest_writel(s, REG_COMMAND, 0x80000000);
        } else {
            uint32_t raw[] = { mode == 5 ? 32 : 8, 1, 0, mode == 5,
                               0, 0, 0, 0, 0 };

            kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
            kona_command(s, 0x6, raw, ARRAY_SIZE(raw));
            if (mode == 5) {
                qtest_writel(s, KONA_REG(0x100000), 0x20000380);
                qtest_memwrite(s, 0x380000, (uint8_t[]) { 0x80, 0, 0, 0 }, 4);
                texture_dma_packet(s, 0, 4, 4, 0);
                g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
            } else {
                qtest_writel(s, REG_COMMAND, 0x80000000);
            }
        }
    } else if (mode == 7) {
        kona_command(s, 0x38,
                     (uint32_t[]) { 0, 0, 1, 1, 0, 0x1800, 0x80000000 }, 7);
    } else {
        kona_command(s, 0x3a, h, ARRAY_SIZE(h));
        if (!dma) {
            qtest_writel(s, REG_COMMAND, scalar ? source[0] : 0xffffffff);
        }
        if (mode == 15) {
            return_context_deactivate(s);
            attrib_context_fresh(s, SAVE_AREA + 0x20000);
            kona_command_f(s, 0x80, (float[]) { 0.25, 0.75 }, 2);
            return_context_deactivate(s);
            return_context_restore(s, SAVE_AREA);
        }
        if (mode == 13 || mode == 14) {
            dir = g_dir_make_tmp("sn0-kona-raster-texture-XXXXXX", &err);
            g_assert_no_error(err);
            uri = g_strdup_printf("unix:%s/migrate.sock", dir);
            to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
            sgi_migrate_stopped(from, uri);
            s = to;
        }
        if (dma) {
            uint8_t bytes[16];
            unsigned count = scalar ? 16 : 4;

            memset(bytes, 255, sizeof(bytes));
            if (scalar) {
                for (unsigned c = 0; c < 4; c++) {
                    stl_be_p(bytes + c * 4, source[c]);
                }
            }
            qtest_writel(s, KONA_REG(0x100000), 0x20000380);
            qtest_memwrite(s, 0x380000, bytes, count);
            texture_dma_packet(s, 0, count, count, 0);
            g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==,
                           mode == 15 ? 0 : 0x100);
        } else if (scalar) {
            for (unsigned c = 1; c < 4; c++) {
                qtest_writel(s, REG_COMMAND, source[c]);
            }
        }
    }
    e->rgb[((127 - 64) * 128 + 64) * 3] = rgb >> 16;
    e->rgb[((127 - 64) * 128 + 64) * 3 + 1] = rgb >> 8;
    e->rgb[((127 - 64) * 128 + 64) * 3 + 2] = rgb;
    assert_window_matches(s, e);
    assert_native_query_word(s, 0xb92, 0x207, false);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_read_pixels_typed(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const uint32_t types[] = { 1, 3, 6 };
    static const uint8_t bytes[4][4] = {
        { 0, 0, 0, 255 }, { 255, 0, 0, 255 },
        { 0, 128, 0, 255 }, { 0, 0, 255, 255 },
    };
    static const uint8_t transferred[4][4] = {
        { 64, 0, 64, 191 }, { 191, 0, 64, 191 },
        { 64, 128, 64, 191 }, { 64, 0, 255, 191 },
    };
    static const uint16_t halves[4][4] = {
        { 16384, 0, 16384, 49151 }, { 49151, 0, 16384, 49151 },
        { 16384, 32896, 16384, 49151 }, { 16384, 0, 65535, 49151 },
    };
    static const uint32_t floats[4][4] = {
        { 0x3e800000, 0, 0x3e800000, 0x3f400000 },
        { 0x3f400000, 0, 0x3e800000, 0x3f400000 },
        { 0x3e800000, 0x3f008081, 0x3e800000, 0x3f400000 },
        { 0x3e800000, 0, 0x3f800000, 0x3f400000 },
    };
    unsigned type = mode >= 9 ? 6 : types[mode % 3];
    unsigned components = mode < 3 ? 3 : 4;
    unsigned scalar_bytes = type == 1 ? 1 : type == 3 ? 2 : 4;
    bool transfer = mode >= 6 && mode != 10;
    uint8_t expected[0x4000], got[0x4000];
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x3a, (uint32_t[]) { 2, 2, 5, 1, 0, 0, 0, 0 }, 8);
    for (unsigned k = 0; k < 4; k++) {
        static const uint32_t pixels[] = {
            0x00000011, 0xff000022, 0x00800033, 0x0000ff44,
        };

        qtest_writel(s, REG_COMMAND, pixels[k]);
    }
    if (transfer) {
        pixel_transfer_set(s, 0xd14, 0.5);
        pixel_transfer_set(s, 0xd15, 0.25);
        pixel_transfer_set(s, 0xd1b, 0.25);
        pixel_transfer_set(s, 0xd1c, 0.5);
        pixel_transfer_set(s, 0xd1d, 0.25);
    }
    if (mode == 10) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
    }
    if (mode == 9) {
        dir = g_dir_make_tmp("sn0-kona-typed-read-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    for (unsigned low = 0; low < 4; low++) {
        memset(expected, 0xa5, sizeof(expected));
        stl_be_p(expected, 1);
        for (unsigned pixel = 0; pixel < 4; pixel++) {
            for (unsigned c = 0; c < components; c++) {
                uint8_t *out = expected + 8 +
                    (pixel * components + c) * scalar_bytes;
                uint8_t v = bytes[pixel][c];

                if (type == 1) {
                    *out = transfer ? transferred[pixel][c] : v;
                } else if (type == 3) {
                    stw_be_p(out, transfer ? halves[pixel][c] : v * 257);
                } else {
                    uint32_t bits = v == 128 ? 0x3f008081 :
                                    v ? 0x3f800000 : 0;

                    stl_be_p(out, transfer ? floats[pixel][c] : bits);
                }
            }
        }
        prepare_refused_reply(s);
        kona_command(s, 0x39,
                     (uint32_t[]) { 64, 64, 2, 2, components == 3 ? 4 : 5,
                                    type, low }, 7);
        g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
        qtest_memread(s, RETURN_PAGE, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void read_dma_clear_notifications(QTestState *s, uint64_t reason_page)
{
    uint8_t hub[8];

    qtest_memset(s, reason_page, 0xa5, 0x4000);
    qtest_memset(s, reason_page, 0, 4);
    qtest_writel(s, REG_INT_CAUSE, 0xffffffff);
    for (unsigned vector = 0x10; vector <= 0x11; vector++) {
        stq_be_p(hub, vector);
        g_assert_true(qtest_memwrite_cpu(s, 0, HUB_LOCAL + 0x90,
                                         hub, sizeof(hub)));
    }
}

static void read_dma_migrate(QTestState *from, QTestState **current,
                              QTestState **to, char **dir)
{
    GError *err = NULL;
    g_autofree char *uri = NULL;

    *dir = g_dir_make_tmp("sn0-kona-read-dma-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", *dir);
    *to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    sgi_migrate_stopped(from, uri);
    *current = *to;
}

static void test_depth_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned type = mode && mode <= 7 ? mode - 1 : 6;
    static const uint32_t source[] = {
        0x407f0000, 0x40ff0000, 0x40007fff, 0x4000ffff,
        0x40000000, 0x40000000, 0x3e800000,
    };
    static const uint8_t first[] = { 129, 64, 128, 64, 128, 64, 64 };
    uint32_t data[2] = { source[type], type == 6 ? 0x3f400000 :
                                                  type == 4 ? 0x7fffffff :
                                                              0xffffffff };
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    uint8_t got[3], expected[3] = { mode ? first[type] : 255,
                                   mode ? type == 6 ? 191 : 255 : 255 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;
    g_autofree Expected *e = g_new(Expected, 1);

    gl_make_current_depth(s, 24);
    gl_window_cleared(s, 0, 0, 0);
    kona_command(s, 0x1a, (uint32_t[]) { 0x100 }, 1);
    if (mode) {
        kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
        kona_command(s, 0xce, (uint32_t[]) { 0x207 }, 1);
        kona_command_f(s, 0x63, (float[]) { 1, 0, 0 }, 3);
        kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
        /* Current colour changes must not replace the raster colour. */
        kona_command_f(s, 0x63, (float[]) { 0, 1, 0 }, 3);
        if (mode >= 8) {
            pixel_transfer_set(s, 0xd1e, -1);
            pixel_transfer_set(s, 0xd1f, 1);
            expected[0] = 191;
            expected[1] = 64;
        }
        if (mode == 9 || mode == 10) {
            kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
            pixel_transfer_set(s, 0xd1e, 0);
            pixel_transfer_set(s, 0xd1f, 0);
            kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
        }
        kona_command(s, 0x3a,
                     (uint32_t[]) { 2, 1, 13, type, 0, mode == 11, 0, 0 }, 8);
        if (mode == 11) {
            uint8_t bytes[8];

            stl_be_p(bytes, data[0]);
            stl_be_p(bytes + 4, data[1]);
            qtest_writel(s, KONA_REG(0x100000), 0x20000380);
            qtest_memwrite(s, 0x380000, bytes, sizeof(bytes));
            texture_dma_packet(s, 0, 8, 8, 0);
        } else {
            qtest_writel(s, REG_COMMAND, data[0]);
            if (mode == 10) {
                read_dma_migrate(from, &s, &to, &dir);
            }
            if (size == 4) {
                qtest_writel(s, REG_COMMAND, data[1]);
            }
        }
        expected_clear(e, 0);
        for (unsigned x = 64; x <= 65; x++) {
            e->rgb[((127 - 64) * 128 + x) * 3] = 255;
        }
        assert_window_matches(s, e);
    }
    if (mode == 12) {
        pixel_transfer_set(s, 0xd1e, 0.5);
        pixel_transfer_set(s, 0xd1f, 0.25);
        expected[0] = 159;
        expected[1] = 96;
    } else {
        pixel_transfer_set(s, 0xd1e, 1);
        pixel_transfer_set(s, 0xd1f, 0);
    }
    if (mode == 14) {
        kona_command_f(s, 0x63, (float[]) { 1, 0, 0 }, 3);
        kona_command_f(s, 0xa9, (float[]) { 1.0f / 64, 0 }, 2);
        kona_command(s, 0x38,
                     (uint32_t[]) { 64, 64, 2, 1, 0, 0x1801, 0x80000000 }, 7);
        expected[2] = expected[1];
        expected[1] = expected[0];
    }
    if (mode == 15) {
        kona_command(s, 0x37, (uint32_t[]) { 0x405 }, 1);
    }
    prepare_refused_reply(s);
    if (mode == 13) {
        uint8_t destination[16], reply[16];
        const uint64_t reason = 0x300000;

        kona_command_kicked(s, 0x1f1,
            (uint32_t[]) { physical_page_word(reason),
                           tagged_offset_word(reason), 0x4000 }, 3);
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        read_dma_clear_notifications(s, reason);
        qtest_memset(s, 0x380000, 0xa5, sizeof(destination));
        kona_command(s, 0x1f5, (uint32_t[]) { 0, 0x9003, 2, 1, 5, 0 }, 6);
        kona_command(s, 0x39, (uint32_t[]) { 64, 64, 1, 2, 13, 1, 3 }, 7);
        kona_command(s, 0x1f6, (uint32_t[]) { 0x100, 0x1234 }, 2);
        memset(reply, 0xa5, sizeof(reply));
        reply[3] = expected[0];
        reply[9] = 255;
        qtest_memread(s, 0x380000, destination, sizeof(destination));
        g_assert_cmpmem(destination, sizeof(destination), reply, sizeof(reply));
        g_assert_cmphex(guest_word(s, reason), ==, 0x100);
        assert_reply_unchanged(s);
    } else {
        unsigned count = mode == 14 ? 3 : 2;

        kona_command(s, 0x39,
                     (uint32_t[]) { 64, 64, count, 1, 13, 1, 0 }, 7);
        assert_reply_posted(s);
        qtest_memread(s, RETURN_PAGE + 8, got, count);
        g_assert_cmpmem(got, count, expected, count);
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_depth_pixel_boundaries(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const uint32_t maxima[] = {
        0x7f, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0xffffffff, 0x3f800000,
    };
    unsigned type = mode < 14 ? mode % 7 : 1;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    uint32_t config[] = { 0, 12, 0, 0, 0, 0, 24, 0, 0, 0 };
    uint8_t expected[8], got[8];
    QTestState *s = qtest_init(MACHINE_ARGS);
    g_autofree Expected *e = g_new(Expected, 1);

    gl_make_current_depth(s, 24);
    gl_window_cleared(s, 0, 0, 0);
    kona_command(s, 0x1a, (uint32_t[]) { 0x100 }, 1);
    if (mode >= 14 && mode != 17) {
        kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
        kona_command(s, 0xce, (uint32_t[]) { 0x207 }, 1);
        kona_command_f(s, 0x63, (float[]) { 1, 0, 0 }, 3);
        kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
        if (mode == 15) {
            pixel_transfer_set(s, 0xd1e, INFINITY);
        }
        if (mode == 16 || mode == 18) {
            config[6] = mode == 18 ? 0 : 16;
            kona_command(s, 0x1, config, ARRAY_SIZE(config));
        }
        kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 13, 6, 0, 0, 0, 0 }, 8);
        qtest_writel(s, REG_COMMAND, fbits(0.25));
        qtest_writel(s, REG_COMMAND, mode == 14 ? 0x7fc00000 : fbits(0.75));
        expected_clear(e, 0);
        if (mode == 18) {
            e->rgb[((127 - 64) * 128 + 64) * 3] = 255;
            e->rgb[((127 - 64) * 128 + 65) * 3] = 255;
        }
        assert_window_matches(s, e);
        pixel_transfer_set(s, 0xd1e, 1);
        config[6] = 24;
        kona_command(s, 0x1, config, ARRAY_SIZE(config));
    }
    if (mode == 17) {
        config[6] = 0;
        kona_command(s, 0x1, config, ARRAY_SIZE(config));
    }
    kona_command(s, 0x33, (uint32_t[]) { 0xd00, mode >= 7 && mode < 14 }, 2);
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]) { 64, 64, 2, 1, 13, type, 2 }, 7);
    if (mode == 17) {
        assert_reply_unchanged(s);
    } else {
        assert_reply_posted(s);
        for (unsigned pixel = 0; pixel < 2; pixel++) {
            for (unsigned k = 0; k < size; k++) {
                unsigned shift = 8 * (mode >= 7 && mode < 14 ?
                                      k : size - 1 - k);

                expected[pixel * size + k] = maxima[type] >> shift;
            }
        }
        qtest_memread(s, RETURN_PAGE + 8, got, 2 * size);
        g_assert_cmpmem(got, 2 * size, expected, 2 * size);
    }
    qtest_quit(s);
}

static void test_stencil_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint8_t got[3], expected[] = { 0x12, 0x34, 0 };
    QTestState *s = qtest_init(MACHINE_ARGS);
    g_autofree Expected *e = g_new(Expected, 1);

    gl_make_current_depth(s, 24);
    stencil_config(s, 8);
    gl_window_cleared(s, 0, 0, 1);
    stencil_clear(s, 0, 255);
    kona_command(s, 0x1a, (uint32_t[]) { 0x100 }, 1);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (mode == 1) {
        kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
        kona_command(s, 0xce, (uint32_t[]) { 0x200 }, 1);
        kona_command(s, 0x24, (uint32_t[]) { 0xb90 }, 1);
        stencil_function(s, 0x200, 0, 255);
        kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
        kona_command(s, 0xc3, (uint32_t[]) { 0x200, fbits(1) }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xde0 }, 1);
        kona_command(s, 0x24, (uint32_t[]) { 0xb60 }, 1);
    }
    kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 12, 1, 0, 0, 0, 0 }, 8);
    qtest_writel(s, REG_COMMAND, 0x12340000);
    expected_clear(e, 0x0000ff);
    assert_window_matches(s, e);
    if (mode == 2) {
        kona_command_f(s, 0xa9, (float[]) { 1.0f / 64, 0 }, 2);
        kona_command(s, 0x38,
                     (uint32_t[]) { 64, 64, 2, 1, 0, 0x1802, 0x80000000 }, 7);
        expected[2] = expected[1];
        expected[1] = expected[0];
    }
    prepare_refused_reply(s);
    kona_command(s, 0x39,
                 (uint32_t[]) { 64, 64, mode == 2 ? 3 : 2, 1, 12, 1, 0 }, 7);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 8, got, mode == 2 ? 3 : 2);
    g_assert_cmpmem(got, mode == 2 ? 3 : 2, expected, mode == 2 ? 3 : 2);
    qtest_quit(s);
}

static void packed_pixel_bytes(uint8_t *out, uint32_t bits, unsigned bytes,
                                 bool swap);

static void test_stencil_pixels_boundaries(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint8_t got[3], expected[] = { 0xa5, 0xa5, 0xa5 };
    uint32_t width = mode == 0 ? 0 : mode == 1 ? 4 : 8;
    uint32_t config[] = { 0, 12, 0, 0, 0, 0, 24, 8, 0, 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    gl_make_current_depth(s, 24);
    stencil_config(s, width);
    gl_window_cleared(s, 0, 0, 1);
    if (width == 8) {
        stencil_clear(s, 0xa5, 255);
    }
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (mode == 2) {
        kona_command(s, 0x20, (uint32_t[]) { 0xf }, 1);
        expected[0] = 0xa2;
        expected[1] = 0xa4;
    } else if (mode == 3) {
        kona_command(s, 0x24, (uint32_t[]) { 0xc11 }, 1);
        kona_command(s, 0xc, (uint32_t[]) { 65, 64, 1, 1 }, 4);
        expected[1] = 0x34;
    } else if (mode == 4) {
        kona_command_f(s, 0x2f, (float[]) { -1, 1 }, 2);
        kona_command_f(s, 0xa9, (float[]) { 2.0f / 64, 0 }, 2);
        expected[0] = 0x34;
        expected[1] = 0x12;
    } else if (mode == 5) {
        pixel_transfer_set(s, 0xd11, -0.0f);
        expected[0] = 0x12;
        expected[1] = 0x34;
    } else if (mode == 6) {
        kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
        pixel_transfer_set(s, 0xd11, 1);
        read_dma_migrate(from, &s, &to, &dir);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
        expected[0] = 0x12;
        expected[1] = 0x34;
    } else if (mode == 7) {
        pixel_transfer_set(s, 0xd12, INFINITY);
    } else if (mode == 8) {
        pixel_transfer_set(s, 0xd13, 2147483648.0f);
    } else if (mode == 9) {
        kona_command(s, 0x37, (uint32_t[]) { 0x405 }, 1);
        expected[0] = 0x12;
        expected[1] = 0x34;
    }
    kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 12, 1, 0, 0, 0, 0 }, 8);
    qtest_writel(s, REG_COMMAND, 0x12340000);
    pixel_transfer_set(s, 0xd12, 0);
    pixel_transfer_set(s, 0xd13, 0);
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]) { 64, 64, 3, 1, 12, 1, 0 }, 7);
    if (mode < 2) {
        assert_reply_unchanged(s);
        prepare_refused_reply(s);
        kona_command(s, 0x3e, (uint32_t[]) { 0xdbdbdbdb }, 1);
        assert_reply_posted(s);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==,
                        mode == 0 ? 0x502 : 0);
        if (mode == 0) {
            kona_command(s, 0x38,
                         (uint32_t[]) { 0, 0, 2, 1, 0, 0x1802, 0x80000000 }, 7);
            prepare_refused_reply(s);
            kona_command(s, 0x3e, (uint32_t[]) { 0xdbdbdbdb }, 1);
            assert_reply_posted(s);
            g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 0x502);
        }
    } else {
        assert_reply_posted(s);
        qtest_memread(s, RETURN_PAGE + 8, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
        kona_command(s, 1, config, ARRAY_SIZE(config));
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void stencil_map_upload(QTestState *s, uint32_t token,
                               const uint32_t *values, unsigned count)
{
    unsigned capacity = count <= 256 ? 256 : count <= 1024 ? 1024 : 4096;

    kona_command(s, token,
                 (uint32_t[]) { 0xc71, count, fbits(count - 1), capacity }, 4);
    for (unsigned k = 0; k < count; k++) {
        qtest_writel(s, REG_COMMAND, values[k]);
    }
}

static void assert_pixel_map_sizes(QTestState *s, const unsigned *counts)
{
    for (unsigned k = 0; k < 9; k++) {
        assert_native_query_word(s, 0xcb1 + k, counts[k], k & 1);
    }
}

static void upload_pixel_map_sizes(QTestState *s, const unsigned *counts)
{
    for (unsigned k = 0; k < 9; k++) {
        unsigned capacity = counts[k] <= 256 ? 256 :
                            counts[k] <= 1024 ? 1024 : 4096;

        kona_command(s, 0x35,
                     (uint32_t[]) { 0xc71 + k, counts[k],
                                    fbits(counts[k] - 1), capacity }, 4);
        for (unsigned at = 0; at < counts[k]; at++) {
            qtest_writel(s, REG_COMMAND, at);
        }
    }
}

static void test_pixel_map_sizes(gconstpointer opaque)
{
    static const unsigned defaults[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    static const unsigned sizes[] = { 4, 2, 8, 16, 32, 3, 5, 1024, 4096 };
    unsigned mode = GPOINTER_TO_UINT(opaque);
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    gl_make_current(s);
    assert_pixel_map_sizes(s, defaults);
    if (mode == 0) {
        goto out;
    }
    upload_pixel_map_sizes(s, sizes);
    assert_pixel_map_sizes(s, sizes);
    if (mode == 2) {
        upload_pixel_map_sizes(s, defaults);
        assert_pixel_map_sizes(s, defaults);
    } else if (mode == 3) {
        /* Invalid entries must leave the published sizes intact. */
        kona_command(s, 0x34,
                     (uint32_t[]) { 0xc76, 2, fbits(1), 256 }, 4);
        qtest_writel(s, REG_COMMAND, fbits(0));
        qtest_writel(s, REG_COMMAND, fbits(NAN));
        kona_command(s, 0x35,
                     (uint32_t[]) { 0xc71, 3, fbits(2), 256 }, 4);
        for (unsigned k = 0; k < 3; k++) {
            qtest_writel(s, REG_COMMAND, 0);
        }
        assert_pixel_map_sizes(s, sizes);
    } else if (mode == 4) {
        kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
        upload_pixel_map_sizes(s, defaults);
        read_dma_migrate(from, &s, &to, &dir);
        assert_pixel_map_sizes(s, defaults);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
        assert_pixel_map_sizes(s, sizes);
    } else if (mode == 5) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        assert_pixel_map_sizes(s, defaults);
        upload_pixel_map_sizes(s, defaults);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
        assert_pixel_map_sizes(s, sizes);
    } else if (mode == 6) {
        qtest_system_reset(s);
        gl_make_current(s);
        assert_pixel_map_sizes(s, defaults);
    } else if (mode == 7) {
        read_dma_migrate(from, &s, &to, &dir);
        assert_pixel_map_sizes(s, sizes);
    } else if (mode == 8) {
        unsigned partial[] = { 4, 2, 8, 16, 32, 7, 5, 1024, 4096 };

        kona_command(s, 0x35,
                     (uint32_t[]) { 0xc76, 7, fbits(6), 256 }, 4);
        qtest_writel(s, REG_COMMAND, 0);
        read_dma_migrate(from, &s, &to, &dir);
        for (unsigned k = 1; k < 7; k++) {
            qtest_writel(s, REG_COMMAND, k);
        }
        assert_pixel_map_sizes(s, partial);
    }
out:
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_stencil_pixel_maps(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t values[] = { 0x71, 0x30, 0x34, 0x3a };
    uint8_t got[2], expected[] = { 0x34, 0x71 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    gl_make_current_depth(s, 24);
    stencil_config(s, 8);
    gl_window_cleared(s, 0, 0, 1);
    stencil_clear(s, 0xa5, 255);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (mode == 0 || mode == 16) {
        for (unsigned k = 0; k < 4; k++) {
            values[k] = fbits(values[k] + 0.25f);
        }
        if (mode == 16) {
            values[2] = fbits(-2.75f);
            expected[0] = 0xfd;
        }
        stencil_map_upload(s, 0x34, values, 4);
    } else if (mode == 3 || mode == 15) {
        expected[0] = expected[1] = 0;
        if (mode == 15) {
            gl_begin(s, 0);
            stencil_map_upload(s, 0x35, values, 4);
            kona_command(s, 0x71, (uint32_t[]) { 0 }, 1);
        }
    } else if (mode == 7) {
        kona_command(s, 0x35, (uint32_t[]) { 0xc71, 4, fbits(3), 256 }, 4);
        qtest_writel(s, REG_COMMAND, values[0]);
        qtest_writel(s, REG_COMMAND, values[1]);
        read_dma_migrate(from, &s, &to, &dir);
        qtest_writel(s, REG_COMMAND, values[2]);
        qtest_writel(s, REG_COMMAND, values[3]);
    } else if (mode == 8) {
        kona_command(s, 0x35,
                     (uint32_t[]) { 0xc71, 4096, fbits(4095), 4096 }, 4);
        for (unsigned k = 0; k < 4096; k++) {
            qtest_writel(s, REG_COMMAND, (k ^ 0x5a) & 255);
        }
        expected[0] = 0x48;
        expected[1] = 0x6e;
    } else {
        stencil_map_upload(s, mode == 2 ? 0x36 : 0x35, values, 4);
    }
    if (mode == 6) {
        kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
        stencil_map_upload(s, 0x35, (uint32_t[]) { 99 }, 1);
        read_dma_migrate(from, &s, &to, &dir);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
    } else if (mode >= 9 && mode <= 14) {
        uint32_t count = mode == 9 ? 3 : mode == 11 ? 0 :
                         mode == 13 ? 4097 : 4;
        uint32_t header[] = { mode == 10 ? 0xffff : 0xc71, count,
                              fbits(count - 1), mode == 14 ? 7 : 256 };

        kona_command(s, mode == 12 ? 0x34 : 0x35, header, 4);
        for (unsigned k = 0; k < count; k++) {
            qtest_writel(s, REG_COMMAND,
                         mode == 12 && k == 3 ? fbits(NAN) :
                         mode == 12 ? fbits(99) : 99);
        }
    } else if (mode == 17) {
        pixel_transfer_set(s, 0xd12, 1);
        pixel_transfer_set(s, 0xd13, 1);
        expected[0] = expected[1] = 0x30;
    }
    pixel_transfer_set(s, 0xd11, mode == 4 || mode == 5 ? 0 : 1);
    kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 12, 1, 0, 0, 0, 0 }, 8);
    qtest_writel(s, REG_COMMAND, 0x12340000);
    if (mode == 4) {
        pixel_transfer_set(s, 0xd11, 1);
    } else if (mode == 5) {
        pixel_transfer_set(s, 0xd11, 1);
        kona_command(s, 0x38,
                     (uint32_t[]) { 64, 64, 2, 1, 0, 0x1802, 0x80000000 }, 7);
    }
    if (mode != 4) {
        pixel_transfer_set(s, 0xd11, 0);
    }
    pixel_transfer_set(s, 0xd12, 0);
    pixel_transfer_set(s, 0xd13, 0);
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]) { 64, 64, 2, 1, 12, 1, 0 }, 7);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 8, got, 2);
    g_assert_cmpmem(got, 2, expected, 2);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void bitmap_stencil_read_source(QTestState *s)
{
    static const uint8_t bytes[20] = {
        2, 3, 4, 7, 8, 9, 11, 12, 13, 1, 2, 3, 5, 6, 9, 10, 11, 14,
    };

    gl_make_current_depth(s, 24);
    stencil_config(s, 8);
    gl_window_cleared(s, 0, 0, 1);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0x3a, (uint32_t[]) { 9, 2, 12, 1, 0, 0, 0, 0 }, 8);
    for (unsigned k = 0; k < sizeof(bytes); k += 4) {
        qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + k));
    }
}

static void test_bitmap_stencil_read(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode >= 24;
    bool lsb = mode == 1 || mode == 3 || (mode >= 5 && mode <= 8) ||
               mode == 10 || mode == 18 || (dma && ((mode & 1) || mode == 32));
    bool refused = (mode >= 11 && mode <= 17) || mode == 19 || mode >= 34;
    unsigned phase = dma ? mode & 3 : 0;
    unsigned gap = dma && mode >= 28 && mode <= 31 ? 5 : 0;
    uint8_t payload[] = { lsb ? 0x6a : 0x56, lsb ? 1 : 0x80,
                          lsb ? 0xad : 0xb5, 0 };
    uint8_t got[32], expected[32];
    uint32_t read[] = { 64, 64, 9, 2, 12, 7, phase };
    uint32_t start[] = { 0, 0x9000 | phase, 4, gap ? 2 : 4, gap, 0 };
    const uint64_t destination = 0x380000, reason_page = 0x300000;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    bitmap_stencil_read_source(s);
    kona_command(s, 0x33, (uint32_t[]) { 0xd01, lsb }, 2);
    if (mode == 2 || mode == 3) {
        kona_command(s, 0x33, (uint32_t[]) { 0xd00, 1 }, 2);
    } else if (mode == 4 || mode == 5) {
        pixel_transfer_set(s, 0xd12, 1);
        pixel_transfer_set(s, 0xd13, 1);
        if (mode == 5) {
            stencil_map_upload(s, 0x35, (uint32_t[]) { 0, 1, 0, 0 }, 4);
            pixel_transfer_set(s, 0xd11, 1);
            memcpy(payload, (uint8_t[]) { 0x95, 0, 0x52, 1 }, 4);
        } else {
            memcpy(payload, (uint8_t[]) { 0xff, 0x80, 0xff, 0x80 }, 4);
        }
    } else if (mode == 6) {
        read_dma_migrate(from, &s, &to, &dir);
    } else if (mode == 7) {
        kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
        kona_command(s, 0x33, (uint32_t[]) { 0xd01, 0 }, 2);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
        memcpy(payload, (uint8_t[]) { 0x56, 0x80, 0xb5, 0 }, 4);
    } else if (mode == 8) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        kona_command(s, 0x33, (uint32_t[]) { 0xd01, 0 }, 2);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    } else if (mode == 9) {
        kona_command(s, 0x33, (uint32_t[]) { 0xd01, 1 }, 2);
        qtest_system_reset(s);
        bitmap_stencil_read_source(s);
    } else if (mode == 10) {
        gl_begin(s, 0);
        kona_command(s, 0x33, (uint32_t[]) { 0xd01, 0 }, 2);
        gl_end(s);
    } else if (mode == 11 || mode == 12 || mode == 17) {
        read[4] = mode == 11 ? 13 : mode == 12 ? 5 : 11;
    } else if (mode == 13) {
        stencil_config(s, 0);
    } else if (mode == 14) {
        kona_command(s, 0x114, (uint32_t[]) { 1 }, 1);
    } else if (mode == 15) {
        read[0] = 127;
    } else if (mode == 16) {
        pixel_transfer_set(s, 0xd12, INFINITY);
    } else if (mode == 18) {
        kona_command(s, 0x33, (uint32_t[]) { 0xd01 }, 1);
        read_dma_migrate(from, &s, &to, &dir);
        qtest_writel(s, REG_COMMAND, 1);
    } else if (mode == 19) {
        read[2] = 8;
        read[3] = 1;
    }
    prepare_refused_reply(s);
    if (dma) {
        kona_command_kicked(s, 0x1f1,
                            (uint32_t[]) { physical_page_word(reason_page),
                                           tagged_offset_word(reason_page),
                                           0x4000 }, 3);
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memset(s, destination, 0xa5, sizeof(expected));
        read_dma_clear_notifications(s, reason_page);
        if (mode == 34) {
            start[3] = 1;
        } else if (mode == 35) {
            read[6] = (phase + 1) & 3;
        }
        kona_command(s, 0x1f5, start, ARRAY_SIZE(start));
        if (mode == 33) {
            read_dma_migrate(from, &s, &to, &dir);
        }
    }
    kona_command(s, 0x39, read, ARRAY_SIZE(read));
    if (dma) {
        assert_reply_unchanged(s);
        memset(expected, 0xa5, sizeof(expected));
        qtest_memread(s, destination, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
        if (mode == 32) {
            read_dma_migrate(from, &s, &to, &dir);
            kona_command(s, 0x33, (uint32_t[]) { 0xd01, 0 }, 2);
            stencil_clear(s, 0, 255);
        }
        kona_command(s, 0x1f6, (uint32_t[]) { 0x100, 0x13579bdf }, 2);
        if (!refused) {
            memcpy(expected + phase, payload, 2);
            memcpy(expected + phase + 2 + gap, payload + 2, 2);
        }
        qtest_memread(s, destination, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
        g_assert_cmphex(guest_word(s, reason_page), ==, refused ? 0 : 0x100);
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & 0x20, ==,
                        refused ? 0 : 0x20);
        assert_reply_unchanged(s);
    } else if (refused) {
        assert_reply_unchanged(s);
    } else {
        assert_reply_posted(s);
        memset(expected, 0xa5, sizeof(expected));
        stl_be_p(expected, 0);
        memcpy(expected + 8, payload, sizeof(payload));
        qtest_memread(s, RETURN_PAGE, got, sizeof(got));
        for (unsigned k = 0; k < sizeof(got); k++) {
            if (got[k] != expected[k]) {
                g_test_message("reply byte %u: expected %02x, received %02x",
                               k, expected[k], got[k]);
            }
        }
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void indexed_colour_maps(QTestState *s, uint32_t token)
{
    static const float values[][8] = {
        { 0, 0, 1, 0 }, { 0, 1 }, { 0, 0, 0, 0, 1, 0, 0, 0 }, { 1 },
    };
    static const unsigned counts[] = { 4, 2, 8, 1 };

    for (unsigned c = 0; c < 4; c++) {
        kona_command(s, token,
                     (uint32_t[]) { 0xc72 + c, counts[c],
                                    fbits(counts[c] - 1), 256 }, 4);
        for (unsigned k = 0; k < counts[c]; k++) {
            uint32_t value = token == 0x34 ? fbits(values[c][k]) :
                values[c][k] ? token == 0x35 ? UINT32_MAX : 0xffff : 0;

            qtest_writel(s, REG_COMMAND, value);
        }
    }
}

static void native_index_visual(QTestState *s, unsigned bits)
{
    gl_make_current_depth(s, 24);
    kona_command(s, 1,
                 (uint32_t[]) { 0, 0, bits, 0, 0, 0, 24, 0, 0, 0 }, 10);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
}

static void assert_native_index_pixels(QTestState *s, unsigned x,
                                        const uint32_t *expected, unsigned n)
{
    uint8_t got[64];

    g_assert_cmpuint(n, <=, sizeof(got) / 4);
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]) { x, 64, n, 1, 11, 5, 0 }, 7);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 8, got, n * 4);
    for (unsigned k = 0; k < n; k++) {
        g_assert_cmphex(ldl_be_p(got + 4 * k), ==, expected[k]);
    }
}

static void native_index_draw(QTestState *s, uint32_t a, uint32_t b)
{
    kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 11, 5, 0, 0, 0, 0 }, 8);
    qtest_writel(s, REG_COMMAND, a);
    qtest_writel(s, REG_COMMAND, b);
}

static void native_index_map(QTestState *s, unsigned token,
                              const uint32_t *values, unsigned n)
{
    kona_command(s, token,
                 (uint32_t[]) { 0xc70, n, fbits(n - 1), 256 }, 4);
    for (unsigned k = 0; k < n; k++) {
        qtest_writel(s, REG_COMMAND, values[k]);
    }
}

static void test_native_index_bitmap(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool unpack_lsb = mode & 1, pack_lsb = mode & 2;
    bool input_dma = mode < 32 && (mode & 4);
    bool output_dma = (mode & 8) || mode == 33;
    unsigned phase = output_dma ? (mode >> 1) & 3 : 0;
    unsigned gap = output_dma && (mode & 16) ? 5 : 0;
    uint8_t input[] = { unpack_lsb ? 0x6a : 0x56,
                        unpack_lsb ? 1 : 0x80,
                        unpack_lsb ? 0xad : 0xb5, 0 };
    uint8_t payload[] = { pack_lsb ? 0x6a : 0x56,
                          pack_lsb ? 1 : 0x80,
                          pack_lsb ? 0xad : 0xb5, 0 };
    uint32_t indices[] = { 0x122, 0x123, 0x122, 0x123, 0x122,
                           0x123, 0x123, 0x122, 0x123 };
    uint8_t got[32], expected[32];
    const uint64_t destination = 0x380000, reason_page = 0x300000;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    native_index_visual(s, mode & 16 ? 8 : 12);
    native_index_map(s, 0x35, (uint32_t[]) { 0x122, 0x123 }, 2);
    pixel_transfer_set(s, 0xd10, 1);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf1, unpack_lsb }, 2);
    qtest_writel(s, KONA_REG(0x100000), 0x20000380);
    if (input_dma) {
        qtest_memwrite(s, destination, input, sizeof(input));
    }
    kona_command(s, 0x3a,
                 (uint32_t[]) { 9, 2, 11, 7, 0, input_dma, 0, 0 }, 8);
    if (mode == 32) {
        read_dma_migrate(from, &s, &to, &dir);
    }
    if (input_dma) {
        texture_dma_packet(s, 0, 4, 4, 0);
    } else {
        qtest_writel(s, REG_COMMAND, ldl_be_p(input));
    }
    pixel_transfer_set(s, 0xd10, 0);
    if (mode & 16) {
        for (unsigned k = 0; k < ARRAY_SIZE(indices); k++) {
            indices[k] &= 0xff;
        }
    }
    assert_native_index_pixels(s, 64, indices, ARRAY_SIZE(indices));
    kona_command(s, 0x33, (uint32_t[]) { 0xd01, pack_lsb }, 2);
    prepare_refused_reply(s);
    memset(expected, 0xa5, sizeof(expected));
    if (output_dma) {
        kona_command_kicked(s, 0x1f1,
                            (uint32_t[]) { physical_page_word(reason_page),
                                           tagged_offset_word(reason_page),
                                           0x4000 }, 3);
        qtest_memset(s, destination, 0xa5, sizeof(expected));
        read_dma_clear_notifications(s, reason_page);
        kona_command(s, 0x1f5,
                     (uint32_t[]) { 0, 0x9000 | phase, 4,
                                    gap ? 2 : 4, gap, 0 }, 6);
    }
    kona_command(s, 0x39,
                 (uint32_t[]) { 64, 64, 9, 2, 11, 7, phase }, 7);
    if (output_dma) {
        assert_reply_unchanged(s);
        qtest_memread(s, destination, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
        if (mode == 33) {
            read_dma_migrate(from, &s, &to, &dir);
            kona_command(s, 0x33, (uint32_t[]) { 0xd01, !pack_lsb }, 2);
            kona_command_f(s, 0x1c, (float[]) { 0 }, 1);
            kona_command(s, 0x1a, (uint32_t[]) { 0x4000 }, 1);
        }
        kona_command(s, 0x1f6, (uint32_t[]) { 0x100, 0x13579bdf }, 2);
        memcpy(expected + phase, payload, 2);
        memcpy(expected + phase + 2 + gap, payload + 2, 2);
        qtest_memread(s, destination, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
        g_assert_cmphex(guest_word(s, reason_page), ==, 0x100);
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & 0x20, ==, 0x20);
        assert_reply_unchanged(s);
    } else {
        assert_reply_posted(s);
        stl_be_p(expected, 0);
        memcpy(expected + 8, payload, sizeof(payload));
        qtest_memread(s, RETURN_PAGE, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_native_index_logic(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque), op = mode & 15;
    uint32_t a = 0x123, b = 0xabc;
    const uint32_t result[] = {
        0, a & b, a & ~b, a, ~a & b, b, a ^ b, a | b,
        ~(a | b), ~(a ^ b), ~b, a | ~b, ~a, ~a | b, ~(a & b), UINT32_MAX,
    };
    uint32_t mask = mode & 16 ? 0xff : 0xfff;
    uint32_t expected[] = { result[op] & mask, result[op] & mask };
    QTestState *s = qtest_init(MACHINE_ARGS);

    native_index_visual(s, mode & 16 ? 8 : 12);
    native_index_draw(s, b, b);
    kona_command(s, 0x24, (uint32_t[]) { 0xbf1 }, 1);
    kona_command(s, 0x2e, (uint32_t[]) { 0x1500 + op }, 1);
    native_index_draw(s, a, a);
    assert_native_index_pixels(s, 64, expected, 2);
    qtest_quit(s);
}

static void test_native_index_state(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t expected[] = { 18, 52 };
    QTestState *s = qtest_init(MACHINE_ARGS);

    native_index_visual(s, 12);
    if (mode == 0) {
        kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
        kona_command(s, 0xce, (uint32_t[]) { 0x207 }, 1);
        kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 13, 6, 0, 0, 0, 0 }, 8);
        qtest_writel(s, REG_COMMAND, fbits(0.25f));
        qtest_writel(s, REG_COMMAND, fbits(0.75f));
        assert_native_index_pixels(s, 64, (uint32_t[]) { 1, 1 }, 2);
        kona_command(s, 0xce, (uint32_t[]) { 0x201 }, 1);
        native_index_draw(s, 18, 52);
        expected[0] = 1;
    } else if (mode == 1) {
        uint8_t got[2];

        kona_command(s, 1,
                     (uint32_t[]) { 0, 0, 12, 0, 0, 0, 24, 8, 0, 0 }, 10);
        stencil_clear(s, 3, 255);
        stencil_function(s, 0x200, 5, 255);
        stencil_operations(s, 0x1e01, 0x1e00, 0x1e00);
        native_index_draw(s, 18, 52);
        assert_native_index_pixels(s, 64, (uint32_t[]) { 0, 0 }, 2);
        stencil_function(s, 0x202, 5, 255);
        stencil_operations(s, 0x1e00, 0x1e00, 0x1e02);
        native_index_draw(s, 18, 52);
        prepare_refused_reply(s);
        kona_command(s, 0x39, (uint32_t[]) { 64, 64, 2, 1, 12, 1, 0 }, 7);
        assert_reply_posted(s);
        qtest_memread(s, RETURN_PAGE + 8, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), ((uint8_t[]) { 6, 6 }), 2);
    } else if (mode == 2) {
        kona_command(s, 1,
                     (uint32_t[]) { 0, 0, 12, 1, 0, 0, 24, 0, 0, 0 }, 10);
        kona_command(s, 0xc4, (uint32_t[]) { 0x404 }, 1);
        native_index_draw(s, 18, 52);
        kona_command(s, 0xc4, (uint32_t[]) { 0x405 }, 1);
        native_index_draw(s, 0xabc, 0x123);
        kona_command(s, 0x37, (uint32_t[]) { 0x405 }, 1);
        assert_native_index_pixels(s, 64, (uint32_t[]) { 0xabc, 0x123 }, 2);
        kona_command(s, 0x37, (uint32_t[]) { 0x404 }, 1);
    } else {
        kona_command(s, 1,
                     (uint32_t[]) { 0, 12, 0, 0, 0, 0, 24, 0, 0, 0 }, 10);
        kona_command(s, 0x3a, (uint32_t[]) { 3, 1, 5, 1, 0, 0, 0, 0 }, 8);
        qtest_writel(s, REG_COMMAND, 0xff0000ff);
        qtest_writel(s, REG_COMMAND, 0x00ff00ff);
        qtest_writel(s, REG_COMMAND, 0xffff00ff);
        kona_command(s, 1,
                     (uint32_t[]) { 0, 0, 12, 0, 0, 0, 24, 0, 0, 0 }, 10);
        native_index_draw(s, 18, 52);
        /* The final untouched pixel still has an RGB tag. */
        prepare_refused_reply(s);
        kona_command(s, 0x39, (uint32_t[]) { 64, 64, 3, 1, 11, 5, 0 }, 7);
        assert_reply_unchanged(s);
    }
    assert_native_index_pixels(s, 64, expected, 2);
    qtest_quit(s);
}

static void native_index_set(QTestState *s, unsigned type, double value)
{
    if (!type) {
        kona_command_d(s, 0x72, &value, 1);
    } else if (type == 1) {
        kona_command_f(s, 0x73, (float[]) { value }, 1);
    } else {
        kona_command(s, 0x74, (uint32_t[]) { (int32_t)value }, 1);
    }
}

static void test_native_index_setters(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned bits = mode < 6 && mode >= 3 ? 8 : 12;
    unsigned type = mode < 6 ? mode % 3 : 1;
    uint32_t expected[] = { 0x123, type == 2 ? 0xffe : 0xffd };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    native_index_visual(s, bits);
    if (mode < 6) {
        gl_begin(s, 0);
        native_index_set(s, type, 291.75);
        gl_vertex2f(s, 0, 0);
        native_index_set(s, type, -2.75);
        gl_vertex2f(s, 1.0f / 64, 0);
        gl_end(s);
    } else {
        native_index_set(s, 1, 291.75);
        if (mode == 6) {
            kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
            native_index_set(s, 1, 1);
            kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
            kona_command(s, 0xce, (uint32_t[]) { 0x207 }, 1);
            kona_command(s, 0x3a,
                         (uint32_t[]) { 2, 1, 13, 6, 0, 0, 0, 0 }, 8);
            qtest_writel(s, REG_COMMAND, fbits(0.25f));
            qtest_writel(s, REG_COMMAND, fbits(0.75f));
            expected[1] = 0x123;
            goto read;
        } else if (mode == 7) {
            kona_command(s, 0xc9, (uint32_t[]) { 1 }, 1);
            native_index_set(s, 1, 1);
            kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
        } else if (mode == 8) {
            return_context_deactivate(s);
            attrib_context_fresh(s, SAVE_AREA + 0x20000);
            native_index_set(s, 1, 1);
            return_context_deactivate(s);
            return_context_restore(s, SAVE_AREA);
        } else if (mode == 9) {
            read_dma_migrate(from, &s, &to, &dir);
        } else if (mode == 10) {
            qtest_writel(s, REG_COMMAND, 0x73);
            read_dma_migrate(from, &s, &to, &dir);
            qtest_writel(s, REG_COMMAND, fbits(291.75f));
        } else if (mode == 11) {
            double value = 291.75;
            uint64_t raw;

            memcpy(&raw, &value, sizeof(raw));
            qtest_writel(s, REG_COMMAND, 0x72);
            qtest_writel(s, REG_COMMAND, raw >> 32);
            read_dma_migrate(from, &s, &to, &dir);
            qtest_writel(s, REG_COMMAND, raw);
        } else if (mode == 12) {
            kona_command(s, 0x75, (uint32_t[]) { 0x12000000 }, 1);
        } else if (mode == 15) {
            qtest_system_reset(s);
            native_index_visual(s, 12);
            expected[0] = 1;
        }
        if (mode == 13) {
            gl_begin(s, 1);
            gl_vertex2f(s, -0.25, 0.5f / 64);
            gl_vertex2f(s, 0.25, 0.5f / 64);
        } else if (mode == 14) {
            gl_begin(s, 4);
            gl_vertex2f(s, -0.5, -0.5);
            gl_vertex2f(s, 0.5, -0.5);
            gl_vertex2f(s, 0, 0.5);
        } else {
            gl_begin(s, 0);
            gl_vertex2f(s, 0, 0);
        }
        gl_end(s);
        assert_native_index_pixels(s, 64, expected, 1);
        goto out;
    }
read:
    for (unsigned k = 0; k < 2; k++) {
        expected[k] &= (1u << bits) - 1;
    }
    assert_native_index_pixels(s, 64, expected, 2);
out:
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_native_packed_index(gconstpointer opaque)
{
    static const struct {
        uint32_t payload, expected;
    } cases[] = {
        { 0x01230000, 0x123 }, { 0xfffd0000, 0xffd },
        { 0x80010000, 1 }, { 0x7fff0000, 0xfff }, { 0, 0 },
        { 0x01000000, 0x234 }, { 0x12000000, 0x234 },
        { 0x80000000, 0x234 }, { 0xff000000, 0x234 },
        { 0x01230001, 0x234 }, { 0x00000001, 0x234 },
    };
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned k = mode % ARRAY_SIZE(cases);
    bool migrate = mode >= ARRAY_SIZE(cases);
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    native_index_visual(s, 12);
    native_index_set(s, 2, 0x234);
    if (migrate) {
        qtest_writel(s, REG_COMMAND, 0x75);
        read_dma_migrate(from, &s, &to, &dir);
        qtest_writel(s, REG_COMMAND, cases[k].payload);
    } else {
        kona_command(s, 0x75, &cases[k].payload, 1);
    }
    gl_begin(s, 0);
    gl_vertex2f(s, 0, 0);
    gl_end(s);
    assert_native_index_pixels(s, 64, &cases[k].expected, 1);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_native_index_sources(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned type = mode % 7;
    unsigned bits = ((mode / 7) & 1) ? 12 : 8;
    bool dma = (mode / 14) & 1, swap = (mode / 28) & 1;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    uint32_t first = type < 2 ? 18 : 0x123;
    uint32_t second = type < 2 ? 52 : 0xbcd;
    uint32_t expected[] = { first & ((1u << bits) - 1),
                            second & ((1u << bits) - 1) };
    uint8_t bytes[12] = { 0 };
    QTestState *s = qtest_init(MACHINE_ARGS);

    native_index_visual(s, bits);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf0, swap }, 2);
    packed_pixel_bytes(bytes + 1, type == 6 ? fbits(first + 0.75f) : first,
                         size, swap);
    packed_pixel_bytes(bytes + 1 + size,
                         type == 6 ? fbits(second + 0.75f) : second,
                         size, swap);
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380001, bytes + 1, 2 * size);
    }
    kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 11, type, 1, dma, 0, 0 }, 8);
    if (dma) {
        texture_dma_packet(s, 1, 2 * size, 2 * size, 0);
    } else {
        for (unsigned k = 0; k < DIV_ROUND_UP(1 + 2 * size, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + k * 4));
        }
    }
    assert_native_index_pixels(s, 64, expected, 2);
    qtest_quit(s);
}

static void test_native_index_read_types(gconstpointer opaque)
{
    unsigned type = GPOINTER_TO_UINT(opaque);
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    uint32_t masks[] = { 0x7f, 0xff, 0x7fff, 0xffff,
                         0x7fffffff, 0xffffffff };
    uint8_t got[8], expected[8];
    QTestState *s = qtest_init(MACHINE_ARGS);

    native_index_visual(s, 12);
    native_index_draw(s, 0xabc, 0x123);
    kona_command(s, 0x33, (uint32_t[]) { 0xd00, 1 }, 2);
    for (unsigned k = 0; k < 2; k++) {
        uint32_t raw = k ? 0x123 : 0xabc;

        packed_pixel_bytes(expected + k * size,
                             type == 6 ? fbits(raw) : raw & masks[type],
                             size, true);
    }
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]) { 64, 64, 2, 1, 11, type, 0 }, 7);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 8, got, 2 * size);
    g_assert_cmpmem(got, 2 * size, expected, 2 * size);
    qtest_quit(s);
}

static void test_native_index_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t values[] = { 0xabc, 0x123, 0x345, 0x678 };
    uint32_t expected[] = { 18, 52, 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;
    bool mapped = mode >= 2 && mode <= 11;

    native_index_visual(s, 12);
    assert_native_query_word(s, 0xcb0, 1, false);
    if (mapped && mode != 5) {
        if (mode == 4) {
            for (unsigned k = 0; k < 4; k++) {
                values[k] = fbits(k == 2 ? -2.75f : 291.25f);
            }
        }
        if (mode == 7) {
            kona_command(s, 0x35,
                         (uint32_t[]) { 0xc70, 4, fbits(3), 256 }, 4);
            qtest_writel(s, REG_COMMAND, values[0]);
            qtest_writel(s, REG_COMMAND, values[1]);
            read_dma_migrate(from, &s, &to, &dir);
            qtest_writel(s, REG_COMMAND, values[2]);
            qtest_writel(s, REG_COMMAND, values[3]);
        } else {
            native_index_map(s, mode == 3 ? 0x36 : mode == 4 ? 0x34 : 0x35,
                               values, 4);
        }
        assert_native_query_word(s, 0xcb0, 4, true);
    }
    if (mapped) {
        pixel_transfer_set(s, 0xd10, 1);
        expected[0] = mode == 5 ? 0 : mode == 4 ? 0xffd : 0x345;
        expected[1] = mode == 5 ? 0 : mode == 4 ? 0x123 : 0xabc;
    }
    if (mode == 0 || mode == 1) {
        kona_command_f(s, 0x1c, (float[]) { mode ? 0xabc : 257.75f }, 1);
        kona_command(s, 0x1a, (uint32_t[]) { 0x4000 }, 1);
        if (!mode) {
            expected[0] = expected[1] = 257;
            goto read;
        }
        kona_command(s, 0x23, (uint32_t[]) { 0xf }, 1);
        expected[0] = 0xab2;
        expected[1] = 0xab4;
    } else if (mode == 6) {
        kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
        native_index_map(s, 0x35, (uint32_t[]) { 0 }, 1);
        read_dma_migrate(from, &s, &to, &dir);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
    } else if (mode == 8) {
        read_dma_migrate(from, &s, &to, &dir);
    } else if (mode == 9 || mode == 10) {
        if (mode == 9) {
            native_index_map(s, 0x34, (uint32_t[]) { fbits(0), fbits(NAN) }, 2);
        } else {
            native_index_map(s, 0x35, (uint32_t[]) { 0, 0, 0 }, 3);
        }
        assert_native_query_word(s, 0xcb0, 4, false);
    } else if (mode == 11) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        assert_native_query_word(s, 0xcb0, 1, false);
        native_index_map(s, 0x35, (uint32_t[]) { 0 }, 1);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
        assert_native_query_word(s, 0xcb0, 4, false);
    } else if (mode == 19) {
        native_index_map(s, 0x35, values, 4);
        qtest_system_reset(s);
        native_index_visual(s, 12);
        assert_native_query_word(s, 0xcb0, 1, false);
    }
    if (mode == 14) {
        kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 11, 6, 0, 0, 0, 0 }, 8);
        qtest_writel(s, REG_COMMAND, fbits(18));
        qtest_writel(s, REG_COMMAND, fbits(NAN));
        expected[0] = expected[1] = 0;
    } else if (mode == 23) {
        uint8_t source[8];

        stl_be_p(source, 18);
        stl_be_p(source + 4, 52);
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, source, sizeof(source));
        kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 11, 5, 0, 1, 0, 0 }, 8);
        read_dma_migrate(from, &s, &to, &dir);
        texture_dma_packet(s, 0, 8, 8, 0);
    } else if (mode == 24) {
        kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 11, 5, 0, 0, 0, 0 }, 8);
        qtest_writel(s, REG_COMMAND, 18);
        read_dma_migrate(from, &s, &to, &dir);
        qtest_writel(s, REG_COMMAND, 52);
    } else {
        native_index_draw(s, 18, 52);
    }
    /* Observe stored indices independently of the enabled input transfer. */
    pixel_transfer_set(s, 0xd10, 0);
    if (mode == 12 || mode == 13) {
        if (mode == 13) {
            native_index_map(s, 0x35, values, 4);
            pixel_transfer_set(s, 0xd10, 1);
        }
        prepare_refused_reply(s);
        kona_command(s, 0x110, (uint32_t[]) { 64, 64, 2, 1 }, 4);
        assert_reply_posted(s);
        kona_command_f(s, 0xa9, (float[]) { 1.0f / 64, 0 }, 2);
        kona_command(s, 0x38,
                     (uint32_t[]) { 64, 64, 2, 1, 0, 0x1800, 0x80000000 }, 7);
        expected[2] = mode == 13 ? 0xabc : 52;
        expected[1] = mode == 13 ? 0x345 : 18;
        pixel_transfer_set(s, 0xd10, 0);
        assert_native_index_pixels(s, 64, expected, 3);
        goto out;
    } else if (mode == 15) {
        uint8_t got[8], rgba[] = { 255, 0, 0, 255, 0, 0, 255, 255 };

        indexed_colour_maps(s, 0x34);
        pixel_transfer_set(s, 0xd10, 1);
        pixel_transfer_set(s, 0xd14, NAN);
        prepare_refused_reply(s);
        kona_command(s, 0x39, (uint32_t[]) { 64, 64, 2, 1, 5, 1, 0 }, 7);
        assert_reply_posted(s);
        qtest_memread(s, RETURN_PAGE + 8, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), rgba, sizeof(rgba));
        goto out;
    } else if (mode == 18) {
        native_index_map(s, 0x35, values, 4);
        pixel_transfer_set(s, 0xd10, 1);
        expected[0] = 0x345;
        expected[1] = 0xabc;
    } else if (mode == 20) {
        kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 5, 1, 0, 0, 0, 0 }, 8);
        qtest_writel(s, REG_COMMAND, 0xffffffff);
        qtest_writel(s, REG_COMMAND, 0xffffffff);
        prepare_refused_reply(s);
        kona_command(s, 0x3e, (uint32_t[]) { 0xdbdbdbdb }, 1);
        assert_reply_posted(s);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 0x502);
    } else if (mode == 21 || mode == 22) {
        if (mode == 21) {
            kona_command(s, 1,
                         (uint32_t[]) { 0, 0, 16, 0, 0, 0, 24, 0, 0, 0 }, 10);
        } else {
            kona_command(s, 0x114, (uint32_t[]) { 1 }, 1);
        }
        prepare_refused_reply(s);
        kona_command(s, 0x39, (uint32_t[]) { 64, 64, 2, 1, 11, 5, 0 }, 7);
        assert_reply_unchanged(s);
        goto out;
    }
read:
    assert_native_index_pixels(s, 64, expected, 2);
out:
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_indexed_colour_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned type = mode < 28 ? mode % 7 : 6;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    bool dma = mode < 28 && ((mode / 7) & 1);
    bool swap = mode < 28 && ((mode / 7) & 2);
    uint32_t first = type == 6 ? fbits(18) : 18;
    uint32_t second = type == 6 ? fbits(52) : 52;
    uint32_t rgb[] = { 0xff0000, 0x0000ff };
    uint8_t bytes[12] = { 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;
    g_autofree Expected *e = g_new(Expected, 1);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    indexed_colour_maps(s, mode == 28 ? 0x35 : mode == 29 ? 0x36 : 0x34);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (mode == 30) {
        first = fbits(2.75f);
        second = fbits(-0.75f);
        rgb[0] = rgb[1] = 0x00ff00;
    } else if (mode == 31) {
        pixel_transfer_set(s, 0xd12, -1);
        pixel_transfer_set(s, 0xd13, 1);
        rgb[0] = 0xff0000;
        rgb[1] = 0x00ff00;
    } else if (mode == 32) {
        pixel_transfer_set(s, 0xd14, 0);
        pixel_transfer_set(s, 0xd19, 1);
    } else if (mode == 33) {
        kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
        kona_command(s, 0x34,
                     (uint32_t[]) { 0xc72, 1, fbits(0), 256 }, 4);
        qtest_writel(s, REG_COMMAND, fbits(0));
        read_dma_migrate(from, &s, &to, &dir);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
    } else if (mode == 35) {
        second = fbits(NAN);
        rgb[0] = rgb[1] = 0;
    } else if (mode == 36) {
        qtest_system_reset(s);
        gl_make_current(s);
        gl_window_cleared(s, 0, 0, 0);
        kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
        rgb[0] = rgb[1] = 0;
    }
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf0, swap }, 2);
    packed_pixel_bytes(bytes + 1, first, size, swap);
    packed_pixel_bytes(bytes + 1 + size, second, size, swap);
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380001, bytes + 1, 2 * size);
    }
    kona_command(s, 0x3a,
                 (uint32_t[]) { 2, 1, 11, type, 1, dma, 0, 0 }, 8);
    if (dma) {
        texture_dma_packet(s, 1, 2 * size, 2 * size, 0);
    } else {
        qtest_writel(s, REG_COMMAND, ldl_be_p(bytes));
        if (mode == 34) {
            read_dma_migrate(from, &s, &to, &dir);
        }
        for (unsigned k = 1; k < DIV_ROUND_UP(1 + 2 * size, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + 4 * k));
        }
    }
    expected_clear(e, 0);
    for (unsigned k = 0; k < 2; k++) {
        unsigned at = ((127 - 64) * 128 + 64 + k) * 3;

        e->rgb[at] = rgb[k] >> 16;
        e->rgb[at + 1] = rgb[k] >> 8;
        e->rgb[at + 2] = rgb[k];
    }
    assert_window_matches(s, e);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_bitmap_index_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool stencil = mode < 32 ? mode & 16 :
                   mode >= 38 ? (mode - 38) & 4 : mode == 37;
    bool dma = mode < 32 ? mode & 1 :
               mode >= 38 ? (mode - 38) & 1 : mode == 33 || mode == 34;
    bool lsb = mode < 32 ? mode & 2 :
               mode >= 38 ? (mode - 38) & 2 : mode >= 32 && mode <= 34;
    unsigned phase = mode < 32 ? (mode >> 2) & 3 : 1;
    bool strided = mode >= 38;
    unsigned skip = strided ? 3 : 0;
    uint8_t bytes[12] = { 0 }, got[27];
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;
    unsigned first = 0;

    gl_make_current_depth(s, 24);
    if (stencil) {
        stencil_config(s, 8);
        stencil_clear(s, 0xa5, 255);
    }
    gl_window_cleared(s, 1, 1, 1);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (!stencil) {
        static const float maps[][4] = {
            { 1, 0, 1, 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 1, 1, 1, 1 },
        };

        for (unsigned c = 0; c < 4; c++) {
            kona_command(s, 0x34, (uint32_t[]) { 0xc72 + c, 4,
                                                fbits(3), 256 }, 4);
            for (unsigned k = 0; k < 4; k++) {
                qtest_writel(s, REG_COMMAND, fbits(maps[c][k]));
            }
        }
    }
    if (mode == 35) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
    } else if (mode == 36) {
        pixel_transfer_set(s, 0xd12, 1);
        pixel_transfer_set(s, 0xd13, 1);
    } else if (mode == 37) {
        stencil_map_upload(s, 0x35, (uint32_t[]) { 0x24, 0x81 }, 2);
        pixel_transfer_set(s, 0xd11, 1);
    }
    kona_command(s, 0x33, (uint32_t[]) { 0xcf1, lsb }, 2);
    if (strided) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 24 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf4, skip }, 2);
    }
    for (unsigned row = 0; row < 2; row++) {
        if (strided) {
            bytes[phase + row * 4] = lsb ? 7 : 0xe0;
        }
        for (unsigned x = 0; x < 9; x++) {
            if ((x + row) & 1) {
                unsigned bit = x + skip;

                bytes[phase + row * 4 + bit / 8] |=
                    1 << (lsb ? bit % 8 : 7 - bit % 8);
            }
        }
    }
    kona_command(s, 0x3a,
                 (uint32_t[]) { 9, 2, stencil ? 12 : 11, 7,
                                phase, dma, strided, 0 }, 8);
    if (!dma) {
        qtest_writel(s, REG_COMMAND, ldl_be_p(bytes));
        first = 1;
    }
    if (mode == 32 || mode == 33) {
        read_dma_migrate(from, &s, &to, &dir);
    }
    if (mode == 34) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf1, !lsb }, 2);
    }
    if (dma) {
        unsigned length = DIV_ROUND_UP(phase + 6, 4) * 4;

        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, bytes, length);
        texture_dma_packet(s, strided ? phase : 0,
                           strided ? 4 : length, strided ? 2 : length,
                           strided ? 2 : 0);
    } else {
        unsigned words = DIV_ROUND_UP(phase + 6, 4);

        for (unsigned k = first; k < words; k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + 4 * k));
        }
    }
    pixel_transfer_set(s, 0xd11, 0);
    pixel_transfer_set(s, 0xd12, 0);
    pixel_transfer_set(s, 0xd13, 0);
    for (unsigned row = 0; row < 2; row++) {
        prepare_refused_reply(s);
        kona_command(s, 0x39,
                     (uint32_t[]) { 64, 64 + row, 9, 1,
                                    stencil ? 12 : 4, 1, 0 }, 7);
        assert_reply_posted(s);
        qtest_memread(s, RETURN_PAGE + 8, got, stencil ? 9 : 27);
        for (unsigned x = 0; x < 9; x++) {
            unsigned bit = (x + row) & 1;

            if (stencil) {
                g_assert_cmphex(got[x], ==, mode == 37 ?
                                (bit ? 0x81 : 0x24) : bit);
            } else {
                if (mode == 36) {
                    bit = 1;
                }
                g_assert_cmphex(got[x * 3], ==, bit ? 0 : 255);
                g_assert_cmphex(got[x * 3 + 1], ==, 0);
                g_assert_cmphex(got[x * 3 + 2], ==, bit ? 255 : 0);
            }
        }
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_indexed_texture_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned type = mode < 28 ? mode % 7 : 6;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    bool dma = (mode < 28 && ((mode / 7) & 1)) || mode == 34 || mode == 40;
    bool swap = mode < 28 && ((mode / 7) & 2);
    unsigned stride = (2 * size + 3) & ~3u;
    unsigned span = stride + 2 * size, words = DIV_ROUND_UP(span, 4);
    uint32_t h[20], expected[] = { 0xff0000ff, 0x0000ffff };
    uint8_t bytes[32] = { 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    gl_make_current(s);
    texture_native_bind(s, 0);
    kona_command(s, 0x174, texture_header, ARRAY_SIZE(texture_header));
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, texture_source[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    if (mode != 37) {
        indexed_colour_maps(s, mode == 28 ? 0x35 : mode == 29 ? 0x36 : 0x34);
    } else {
        memset(expected, 0, sizeof(expected));
    }
    pixel_transfer_set(s, 0xd10, mode == 38 ? 0 : 1);
    pixel_transfer_set(s, 0xd14, 0);
    pixel_transfer_set(s, 0xd19, 1);
    if (mode == 30) {
        expected[0] = expected[1] = 0x00ff00ff;
    } else if (mode == 31) {
        pixel_transfer_set(s, 0xd12, -1);
        pixel_transfer_set(s, 0xd13, 1);
        expected[1] = 0x00ff00ff;
    }
    if (mode == 39) {
        pixel_transfer_set(s, 0xd14, NAN);
    }
    for (unsigned k = 0; k < 4; k++) {
        unsigned at = (k / 2) * stride + (k % 2) * size;
        uint32_t value = k % 2 ? 52 : 18;

        if (type == 6) {
            value = mode == 30 ? fbits(k % 2 ? -0.75f : 2.75f) : fbits(value);
            if (mode == 36 && k == 3) {
                value = fbits(NAN);
            }
        }
        for (unsigned b = 0; b < size; b++) {
            bytes[at + b] = value >> (8 * (swap ? b : size - 1 - b));
        }
    }
    memcpy(h, texture_header, sizeof(h));
    h[3] = 11;
    h[4] = type;
    h[6] = dma;
    h[7] = 0;
    h[17] |= dma ? 4 : 0;
    kona_command(s, 0x33, (uint32_t[]) { 0xcf0, swap }, 2);
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    if (!dma) {
        qtest_writel(s, REG_COMMAND, ldl_be_p(bytes));
        if (mode == 35 || mode == 41) {
            for (unsigned k = 1; k < words; k++) {
                qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + k * 4));
            }
        }
    }
    if (mode == 40 || mode == 41) {
        kona_command(s, 0x34, (uint32_t[]) { 0xc72, 2, fbits(1), 256 }, 4);
        qtest_writel(s, REG_COMMAND, fbits(0));
    }
    if ((mode >= 33 && mode <= 35) || mode >= 39) {
        read_dma_migrate(from, &s, &to, &dir);
    }
    if (mode == 40 || mode == 41) {
        qtest_writel(s, REG_COMMAND, fbits(0));
    }
    if (mode == 34) {
        kona_command(s, 0x34, (uint32_t[]) { 0xc72, 1, fbits(0), 256 }, 4);
        qtest_writel(s, REG_COMMAND, fbits(0));
        pixel_transfer_set(s, 0xd12, 10);
        pixel_transfer_set(s, 0xd13, 10);
    }
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, bytes, span);
        texture_dma_packet(s, 0, span, span, 0);
    } else if (mode != 35 && mode != 41) {
        for (unsigned k = 1; k < words; k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + k * 4));
        }
    }
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2, texture_source[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2,
                             mode == 36 ? texture_source[k] : expected[k % 2]);
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void texture_shadow_bind(QTestState *s, unsigned target,
                                  unsigned compare, unsigned ambient,
                                  bool linear)
{
    uint32_t record[22] = { 0x44, 0x1010, 256u << 10, 0x11, 0, ambient };
    uint32_t bind[5] = { 0, (target << 28) | (63 << 12),
        (0xd << 14) | (compare << 10) | (linear ? 0x24 : 0),
        0xf01 | (target == 2 ? 0x10 : 0), 0 };

    kona_command(s, 0x1d5, bind, ARRAY_SIZE(bind));
    kona_command(s, 0x179, record, ARRAY_SIZE(record));
}

static void test_depth_texture_pixels(gconstpointer opaque)
{
    static const double tri[3][3] = {
        { -.8, -.8, 0 }, { .8, -.8, 0 }, { 0, .8, 0 },
    };
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode == 1 || mode == 3 || mode == 5;
    bool one_d = mode == 11;
    uint32_t h[20], raw[4] = {
        0x7fffffff, 0x80000000, 0x7fffffff, 0x80000000,
    };
    uint32_t expected[4];
    unsigned words = one_d ? 2 : 4;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    memcpy(h, texture_header, sizeof(h));
    h[0] = one_d ? 0xde0 : 0xde1;
    h[2] = one_d ? 1 : 2;
    h[3] = 13;
    h[4] = 5;
    h[9] = 0xd;
    h[14] = one_d ? 0 : 1;
    h[17] = 0x2200;
    /* Establish an old depth image so a late invalid source must be atomic. */
    texture_shadow_bind(s, one_d ? 1 : 2, 2, 0, false);
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    for (unsigned k = 0; k < words; k++) {
        qtest_writel(s, REG_COMMAND, raw[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    memcpy(expected, raw, sizeof(expected));
    h[6] = dma;
    h[17] |= dma ? 4 : 0;
    if (mode == 3 || mode == 5) {
        pixel_transfer_set(s, 0xd1e, 0);
        pixel_transfer_set(s, 0xd1f, .25);
        for (unsigned k = 0; k < 4; k++) {
            expected[k] = 0x40000000;
        }
    } else if (mode == 6) {
        pixel_transfer_set(s, 0xd14, NAN);
        pixel_transfer_set(s, 0xd15, NAN);
    } else if (mode == 7) {
        h[4] = 6;
        raw[0] = raw[1] = raw[2] = fbits(1);
        raw[3] = fbits(NAN);
    } else if (mode == 8) {
        h[4] = 1;
        words = 2;
        raw[0] = raw[1] = 0x40c00000;
        for (unsigned k = 0; k < 4; k++) {
            expected[k] = k & 1 ? 0xc0c0c0c0 : 0x40404040;
        }
    } else if (mode == 9) {
        h[4] = 3;
        words = 2;
        raw[0] = raw[1] = 0x004000c0;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
        for (unsigned k = 0; k < 4; k++) {
            expected[k] = k & 1 ? 0xc000c000 : 0x40004000;
        }
    } else if (mode == 10) {
        h[4] = 6;
        pixel_transfer_set(s, 0xd1e, .25);
        raw[0] = raw[2] = fbits(1);
        raw[1] = raw[3] = fbits(3);
        for (unsigned k = 0; k < 4; k++) {
            expected[k] = k & 1 ? 0xbfffffff : 0x40000000;
        }
    }
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    if (!dma) {
        unsigned first = mode == 4 ? words : 1;

        for (unsigned k = 0; k < first; k++) {
            qtest_writel(s, REG_COMMAND, raw[k]);
        }
    }
    if (mode >= 2 && mode <= 4) {
        read_dma_migrate(from, &s, &to, &dir);
    }
    if (dma) {
        uint8_t bytes[16];

        for (unsigned k = 0; k < words; k++) {
            stl_be_p(bytes + 4 * k, raw[k]);
        }
        if (mode == 3 || mode == 5) {
            pixel_transfer_set(s, 0xd1e, 1);
            pixel_transfer_set(s, 0xd1f, 0);
        }
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, bytes, words * 4);
        texture_dma_packet(s, 0, words * 4, words * 4, 0);
    } else if (mode != 4) {
        for (unsigned k = 1; k < words; k++) {
            qtest_writel(s, REG_COMMAND, raw[k]);
        }
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    for (unsigned k = 0; k < (one_d ? 2 : 4); k++) {
        assert_texture_pixel(s, k % 2, k / 2, expected[k]);
    }
    if (mode == 16) {
        uint32_t record[22] = { 0x44, 0x1010, 256u << 10, 0x11 };

        kona_command(s, 0x1d5, (uint32_t[]) { 0, 0x20003000,
                                            0xd << 14, 0x11, 1 }, 5);
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
        assert_texture_pixel(s, 0, 0, 0);
    } else {
        texture_shadow_bind(s, one_d ? 1 : 2,
                            mode == 12 ? 1 : (mode == 15 || mode == 18) ? 3 : 2,
                            mode == 13 || mode == 14 ? 31 : 0, mode == 14);
    }
    if (mode == 18) {
        read_dma_migrate(from, &s, &to, &dir);
    }
    kona_command(s, 0x24, (uint32_t[]) { one_d ? 0xde0 : 0xde1 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    kona_command_f(s, 0x88, (float[]) {
        mode == 14 ? 1 : .5, .5, mode == 17 ? 3 : 1, 2,
    }, 4);
    gl_triangle_z(s, tri, 0);
    assert_stencil_colour(s, mode >= 15 ?
                             0x0000ff : mode == 12 ? 0 :
                             mode == 14 ? 0xbebebe : 0xffffff);
    if (mode < 15) {
        kona_command_f(s, 0x88, (float[]) { 1.5, .5, 1, 2 }, 4);
        gl_triangle_z(s, tri, 0);
        assert_stencil_colour(s, (mode == 3 || mode == 5 || mode == 12) ?
                                 0xffffff : mode == 13 || mode == 14 ?
                                 0x7d7d7d : 0);
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_bitmap_texture_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode < 16 ? mode & 1 : mode >= 26 ? (mode - 26) & 1 :
               mode == 17 || mode == 19 || mode == 24 || mode == 25;
    bool lsb = mode < 16 ? mode & 2 : mode >= 26 ? (mode - 26) & 2 : true;
    unsigned phase = mode < 16 ? (mode >> 2) & 3 : 1;
    unsigned skip = mode >= 34 && mode < 42 ? 10 :
                    mode >= 26 && mode < 42 ? 7 : 0;
    unsigned source_phase = (phase + skip / 8) & 3;
    unsigned row_bytes = (2 + (skip & 7) + 7) / 8;
    unsigned words = DIV_ROUND_UP(source_phase + 4 + row_bytes, 4);
    uint32_t h[20], expected[] = { 0xff0000ff, 0x0000ffff };
    uint8_t bytes[12] = { 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    gl_make_current(s);
    texture_native_bind(s, 0);
    kona_command(s, 0x174, texture_header, ARRAY_SIZE(texture_header));
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, texture_source[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    if (mode != 21) {
        static const float maps[][4] = {
            { 1, 0, 1, 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 1, 1, 1, 1 },
        };

        for (unsigned c = 0; c < 4; c++) {
            kona_command(s, 0x34, (uint32_t[]) { 0xc72 + c, 4,
                                                fbits(3), 256 }, 4);
            for (unsigned k = 0; k < 4; k++) {
                qtest_writel(s, REG_COMMAND, fbits(maps[c][k]));
            }
        }
    } else {
        memset(expected, 0, sizeof(expected));
    }
    pixel_transfer_set(s, 0xd10, 1);
    pixel_transfer_set(s, 0xd14, 0);
    pixel_transfer_set(s, 0xd19, 1);
    if (mode == 22) {
        pixel_transfer_set(s, 0xd12, 1);
        pixel_transfer_set(s, 0xd13, 1);
        expected[0] = expected[1];
    }
    if (mode == 23) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
    }
    kona_command(s, 0x33, (uint32_t[]) { 0xcf1, lsb }, 2);
    if (skip) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 24 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf4, skip }, 2);
    }
    for (unsigned row = 0; row < 2; row++) {
        unsigned bit = 1 + (skip & 7);

        bytes[source_phase + row * 4 + bit / 8] =
            1 << (lsb ? bit % 8 : 7 - bit % 8);
    }
    memcpy(h, texture_header, sizeof(h));
    h[3] = 11;
    h[4] = 7;
    h[5] = phase;
    h[6] = dma;
    h[7] = skip != 0;
    h[17] |= dma ? 4 : 0;
    if (mode >= 42) {
        h[9] = mode == 42 ? 0xe : mode == 43 ? 0xd : 8;
        h[17] = 0x2200 | (dma ? 4 : 0);
        if (mode == 42) {
            expected[0] = expected[1] = 0x000000ff;
        } else {
            expected[0] = 0xff0000ff;
            expected[1] = 0x000000ff;
        }
    }
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    if (!dma) {
        qtest_writel(s, REG_COMMAND, ldl_be_p(bytes));
        if (mode == 18 || mode == 20) {
            for (unsigned k = 1; k < words; k++) {
                qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + k * 4));
            }
        }
    }
    if (mode == 19 || mode == 20) {
        kona_command(s, 0x34, (uint32_t[]) { 0xc72, 2, fbits(1), 256 }, 4);
        qtest_writel(s, REG_COMMAND, fbits(0));
    }
    if (mode >= 16 && mode <= 20) {
        read_dma_migrate(from, &s, &to, &dir);
    }
    if (mode == 19 || mode == 20) {
        qtest_writel(s, REG_COMMAND, fbits(0));
    }
    if (mode == 24) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf1, !lsb }, 2);
        pixel_transfer_set(s, 0xd12, 10);
        kona_command(s, 0x34, (uint32_t[]) { 0xc72, 1, fbits(0), 256 }, 4);
        qtest_writel(s, REG_COMMAND, fbits(0));
    }
    if (dma) {
        unsigned length = words * 4;

        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, bytes, length);
        unsigned total = mode == 25 ? length - 1 :
                         skip ? row_bytes * 2 : length;

        texture_dma_packet(s, skip ? source_phase : 0, total,
                           skip ? row_bytes : length, skip ? 4 - row_bytes : 0);
    } else if (mode != 18 && mode != 20) {
        for (unsigned k = 1; k < words; k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + k * 4));
        }
    }
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2, texture_source[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    if (mode >= 42) {
        uint32_t record[22] = { 0x44, 0x101010, 256u << 10 };
        unsigned index = mode == 42 ? 17 : mode == 43 ? 0 : 1;

        kona_command(s, 0x1d5, (uint32_t[]) { 0, (2u << 28) | (index << 12),
                                            h[9] << 14, 0x11, 1 }, 5);
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
    }
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2,
                             mode == 25 ? texture_source[k] : expected[k % 2]);
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_bitmap_texture_dimensions(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool volume = mode >= 2, dma = mode == 1 || mode == 3 || mode == 5;
    uint32_t h[20];
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    static const float maps[][2] = { { 1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 } };

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    if (volume) {
        texture_volume_bind(s, false, false);
    } else {
        uint32_t record[22] = { 0x43, 0x101010, 256u << 10, 0x23 };

        kona_command(s, 0x106, (uint32_t[]) { 0, (1u << 28) | (41 << 12),
                                            0x30 << 14, 1, 1 }, 5);
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
    }
    for (unsigned c = 0; c < 4; c++) {
        kona_command(s, 0x34, (uint32_t[]) { 0xc72 + c, 2,
                                            fbits(1), 256 }, 4);
        qtest_writel(s, REG_COMMAND, fbits(maps[c][0]));
        qtest_writel(s, REG_COMMAND, fbits(maps[c][1]));
    }
    for (unsigned z = 0; z < (volume ? 2 : 1); z++) {
        unsigned count = volume ? 8 : 4;
        uint8_t bits = volume ? z ? 0xc0 : 0 : 0x40;
        uint8_t bytes[] = { 0, 0, 0, bits, 0, 0, 0, bits };

        memcpy(h, texture_header, sizeof(h));
        h[0] = volume ? 0x806f : 0xde0;
        h[2] = volume ? 2 : 1;
        h[3] = 11;
        h[4] = 7;
        h[5] = 3;
        h[6] = dma;
        h[7] = 0;
        h[14] = volume ? 1 : 0;
        h[15] = z;
        h[16] = volume ? 0x113 : 0x23;
        h[17] = (volume ? 0x1141 : 0x2181) | (dma ? 4 : 0);
        h[18] = volume ? 0x55 : 0;
        h[19] = volume ? 0x66 : 0;
        kona_command(s, 0x174, h, ARRAY_SIZE(h));
        if (!dma) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes));
        }
        if ((mode == 4 || mode == 5) && z == 1) {
            read_dma_migrate(from, &s, &to, &dir);
        }
        if (dma) {
            qtest_writel(s, KONA_REG(0x100000), 0x20000380);
            qtest_memwrite(s, 0x380000, bytes, count);
            texture_dma_packet(s, 0, count, count, 0);
        } else if (volume) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + 4));
        }
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    }
    if (mode == 6) {
        read_dma_migrate(from, &s, &to, &dir);
    }
    kona_command(s, 0x24, (uint32_t[]) { volume ? 0x806f : 0xde0 }, 1);
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    for (unsigned colour = 0; colour < 2; colour++) {
        kona_command_f(s, 0x84,
                       (float[]) { volume ? 0.25 : colour ? 0.75 : 0.25,
                                   0.25, colour ? 0.75 : 0.25 }, 3);
        gl_triangle_z(s, tri, 0);
        assert_stencil_colour(s, colour ? 0x0000ff : 0xff0000);
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void colour_map_upload(QTestState *s, unsigned component,
                              uint32_t token, const float *values,
                              unsigned count)
{
    unsigned capacity = count <= 256 ? 256 : count <= 1024 ? 1024 : 4096;

    kona_command(s, token,
                 (uint32_t[]) { 0xc76 + component, count,
                                fbits(count - 1), capacity }, 4);
    for (unsigned k = 0; k < count; k++) {
        uint32_t value = token == 0x34 ? fbits(values[k]) :
            values[k] * (token == 0x35 ? 4294967295.0 : 65535.0) + 0.5;

        qtest_writel(s, REG_COMMAND, value);
    }
}

static void colour_maps(QTestState *s, uint32_t token)
{
    static const float values[][3] = {
        { 1, 0.5, 0 }, { 0, 1 }, { 1, 0, 1 }, { 1 },
    };
    static const unsigned counts[] = { 3, 2, 3, 1 };

    for (unsigned k = 0; k < 4; k++) {
        colour_map_upload(s, k, token, values[k], counts[k]);
    }
}

static void test_colour_pixel_maps(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint8_t got[6], expected[] = { 0x80, 0xff, 0xff, 0, 0, 0 };
    uint32_t data[] = { 0x4080c0ff, 0xc04080ff };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;
    bool texture = (mode >= 3 && mode <= 6) || mode == 20 || mode == 21;
    bool dma = mode == 4 || mode == 6 || mode == 19 || mode == 20;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (mode != 9) {
        colour_maps(s, mode == 10 ? 0x36 : mode == 11 ? 0x35 : 0x34);
    } else {
        memset(expected, 0, sizeof(expected));
    }
    pixel_transfer_set(s, 0xd10, mode == 1 || mode == 2 ? 0 : 1);
    if (texture) {
        uint32_t h[20];
        uint8_t bytes[16];

        texture_native_bind(s, 0);
        memcpy(h, texture_header, sizeof(h));
        h[6] = dma;
        h[7] = 0;
        h[17] |= dma ? 4 : 0;
        kona_command(s, 0x174, h, ARRAY_SIZE(h));
        if (!dma) {
            qtest_writel(s, REG_COMMAND, data[0]);
            if (mode == 21) {
                for (unsigned k = 1; k < 4; k++) {
                    qtest_writel(s, REG_COMMAND, data[0]);
                }
            }
        }
        if (mode == 20 || mode == 21) {
            kona_command(s, 0x34,
                         (uint32_t[]) { 0xc76, 3, fbits(2), 256 }, 4);
            qtest_writel(s, REG_COMMAND, fbits(0));
            read_dma_migrate(from, &s, &to, &dir);
            qtest_writel(s, REG_COMMAND, fbits(0));
            qtest_writel(s, REG_COMMAND, fbits(0));
        }
        if (mode == 5 || mode == 6) {
            read_dma_migrate(from, &s, &to, &dir);
        }
        if (dma) {
            colour_map_upload(s, 0, 0x34, (float[]) { 0 }, 1);
            pixel_transfer_set(s, 0xd10, 0);
            for (unsigned k = 0; k < 4; k++) {
                stl_be_p(bytes + 4 * k, data[0]);
            }
            qtest_writel(s, KONA_REG(0x100000), 0x20000380);
            qtest_memwrite(s, 0x380000, bytes, sizeof(bytes));
            texture_dma_packet(s, 0, sizeof(bytes), sizeof(bytes), 0);
        } else if (mode != 21) {
            for (unsigned k = 1; k < 4; k++) {
                qtest_writel(s, REG_COMMAND, data[0]);
            }
        }
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
        for (unsigned k = 0; k < 4; k++) {
            assert_texture_pixel(s, k % 2, k / 2, 0x80ffffff);
        }
        goto out;
    }
    if (mode == 7) {
        kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
        colour_map_upload(s, 0, 0x34, (float[]) { 0 }, 1);
        pixel_transfer_set(s, 0xd10, 0);
        read_dma_migrate(from, &s, &to, &dir);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
    } else if (mode == 8) {
        kona_command(s, 0x34,
                     (uint32_t[]) { 0xc76, 3, fbits(2), 256 }, 4);
        qtest_writel(s, REG_COMMAND, fbits(1));
        read_dma_migrate(from, &s, &to, &dir);
        qtest_writel(s, REG_COMMAND, fbits(0.5));
        qtest_writel(s, REG_COMMAND, fbits(0));
    } else if (mode == 12) {
        pixel_transfer_set(s, 0xd14, 2);
        pixel_transfer_set(s, 0xd1b, -0.5);
        expected[0] = 0x80;
        expected[1] = 0xff;
        expected[2] = 0;
        expected[3] = 0;
        expected[4] = 0;
        expected[5] = 0xff;
    } else if (mode == 13) {
        pixel_transfer_set(s, 0xd14, 10);
        pixel_transfer_set(s, 0xd19, -10);
        expected[0] = expected[1] = 0;
        expected[2] = 0xff;
    } else if (mode == 14) {
        pixel_transfer_set(s, 0xd10, -0.0f);
        uint8_t plain[] = { 0x40, 0x80, 0xc0, 0xc0, 0x40, 0x80 };

        memcpy(expected, plain, sizeof(expected));
    } else if (mode == 15) {
        colour_map_upload(s, 0, 0x34, (float[]) { 0, NAN, 1 }, 3);
    } else if (mode == 16) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        colour_map_upload(s, 0, 0x34, (float[]) { 0 }, 1);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    } else if (mode == 17) {
        qtest_system_reset(s);
        gl_make_current(s);
        gl_window_cleared(s, 0, 0, 0);
        kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
        pixel_transfer_set(s, 0xd10, 1);
        memset(expected, 0, sizeof(expected));
    } else if (mode == 18) {
        colour_map_upload(s, 3, 0x34, (float[]) { 0 }, 1);
        kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
        kona_command(s, 0xca, (uint32_t[]) { 0x204, fbits(0.5) }, 2);
        memset(expected, 0, sizeof(expected));
    }
    if (dma) {
        uint8_t bytes[8];

        stl_be_p(bytes, data[0]);
        stl_be_p(bytes + 4, data[1]);
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, bytes, sizeof(bytes));
    }
    kona_command(s, 0x3a, (uint32_t[]) { 2, 1, 5, 1, 0, dma, 0, 0 }, 8);
    if (dma) {
        texture_dma_packet(s, 0, 8, 8, 0);
    } else {
        qtest_writel(s, REG_COMMAND, data[0]);
        qtest_writel(s, REG_COMMAND, data[1]);
    }
    if (mode == 1 || mode == 2) {
        pixel_transfer_set(s, 0xd10, 1);
    }
    if (mode == 2) {
        kona_command_f(s, 0xa9, (float[]) { 1.0f / 64, 0 }, 2);
        kona_command(s, 0x38,
                     (uint32_t[]) { 64, 64, 2, 1, 0, 0x1800, 0x80000000 }, 7);
    }
    if (mode != 1) {
        pixel_transfer_set(s, 0xd10, 0);
    }
    pixel_transfer_set(s, 0xd14, 1);
    pixel_transfer_set(s, 0xd19, 0);
    pixel_transfer_set(s, 0xd1b, 0);
    prepare_refused_reply(s);
    kona_command(s, 0x39,
                 (uint32_t[]) { mode == 2 ? 65 : 64, 64, 2, 1, 4, 1, 0 }, 7);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 8, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
out:
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_colour_map_source_types(gconstpointer opaque)
{
    static const unsigned types[] = { 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12 };
    static const unsigned packed_bits[][4] = {
        { 3, 3, 2, 0 }, { 4, 4, 4, 4 }, { 5, 5, 5, 1 },
        { 8, 8, 8, 8 }, { 10, 10, 10, 2 },
    };
    static const double values[][4] = {
        { 0.375, 0.625, 0.875, 1 }, { 0.875, 0.375, 0.625, 1 },
    };
    static const uint32_t maxima[] = {
        0x7f, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0xffffffff,
    };
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned type = types[mode % ARRAY_SIZE(types)];
    bool dma = (mode / ARRAY_SIZE(types)) & 1;
    bool swap = (mode / ARRAY_SIZE(types)) & 2;
    unsigned size = type < 2 || type == 8 ? 1 :
                    type < 4 || type == 9 || type == 10 ? 2 : 4;
    unsigned pixel_bytes = type >= 8 ? size : 4 * size;
    uint8_t bytes[40] = { 0 }, got[6];
    const uint8_t expected[] = { 0x80, 0xff, 0xff, 0, 0, 0 };
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    colour_maps(s, 0x34);
    pixel_transfer_set(s, 0xd10, 1);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf0, swap }, 2);
    for (unsigned pixel = 0; pixel < 2; pixel++) {
        if (type >= 8) {
            uint32_t packed = 0;
            unsigned shift = size * 8;

            for (unsigned k = 0; k < 4; k++) {
                unsigned bits = packed_bits[type - 8][k];
                unsigned maximum = (1u << bits) - 1;
                unsigned value = values[pixel][k] * maximum + 0.5;

                shift -= bits;
                packed |= value << shift;
            }
            packed_pixel_bytes(bytes + 1 + pixel * pixel_bytes,
                               packed, size, swap);
        } else {
            for (unsigned k = 0; k < 4; k++) {
                uint32_t value = type == 6 ? fbits(values[pixel][k]) :
                    values[pixel][k] * maxima[type] + 0.5;

                packed_pixel_bytes(bytes + 1 + pixel * pixel_bytes + k * size,
                                   value, size, swap);
            }
        }
    }
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380001, bytes + 1, 2 * pixel_bytes);
    }
    kona_command(s, 0x3a,
                 (uint32_t[]) { 2, 1, type == 8 ? 4 : 5, type, 1, dma, 0, 0 },
                 8);
    if (dma) {
        texture_dma_packet(s, 1, 2 * pixel_bytes, 2 * pixel_bytes, 0);
    } else {
        for (unsigned k = 0; k < DIV_ROUND_UP(1 + 2 * pixel_bytes, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + 4 * k));
        }
    }
    pixel_transfer_set(s, 0xd10, 0);
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]) { 64, 64, 2, 1, 4, 1, 0 }, 7);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 8, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    qtest_quit(s);
}

static void test_stencil_pixels_transport(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool matrix = mode < 56;
    unsigned type = matrix ? mode % 7 : 6;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    bool dma = matrix ? (mode / 7) & 1 : mode == 58;
    bool swap = matrix && ((mode / 7) & 2);
    bool output_swap = matrix && ((mode / 7) & 4);
    bool refused = mode == 59;
    const uint32_t first = type == 6 ? 0x41900000 : 0x12;
    const uint32_t second = type == 6 ? 0x42500000 : 0x34;
    uint8_t bytes[12] = { 0 }, got[8], expected[8];
    uint8_t output[] = { 0x12, 0x34 };
    uint32_t header[] = { 2, 1, 12, type, 1, dma, 0, 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;
    g_autofree Expected *e = g_new(Expected, 1);

    gl_make_current_depth(s, 24);
    stencil_config(s, 8);
    gl_window_cleared(s, 0, 0, 1);
    stencil_clear(s, 0xa5, 255);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf0, swap }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xd00, output_swap }, 2);
    packed_pixel_bytes(bytes + 1, first, size, swap);
    packed_pixel_bytes(bytes + 1 + size, refused ? 0x7fc00000 : second,
                         size, swap);
    if (!matrix) {
        pixel_transfer_set(s, 0xd12, 1);
        pixel_transfer_set(s, 0xd13, 3);
        output[0] = 39;
        output[1] = 107;
        if (mode == 56) {
            kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
            pixel_transfer_set(s, 0xd12, 0);
            pixel_transfer_set(s, 0xd13, 0);
            kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
        }
        if (mode == 60) {
            pixel_transfer_set(s, 0xd11, 1);
            output[0] = output[1] = 0;
        }
    }
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380001, bytes + 1, 2 * size);
    }
    kona_command(s, 0x3a, header, ARRAY_SIZE(header));
    if (dma) {
        texture_dma_packet(s, 1, 2 * size, 2 * size, 0);
    } else {
        qtest_writel(s, REG_COMMAND, ldl_be_p(bytes));
        if (mode == 57) {
            read_dma_migrate(from, &s, &to, &dir);
        }
        for (unsigned k = 1; k < DIV_ROUND_UP(1 + 2 * size, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(bytes + 4 * k));
        }
    }
    expected_clear(e, 0x0000ff);
    assert_window_matches(s, e);
    pixel_transfer_set(s, 0xd12, 0);
    pixel_transfer_set(s, 0xd13, 0);
    pixel_transfer_set(s, 0xd11, 0);
    if (refused) {
        output[0] = output[1] = 0xa5;
    }
    for (unsigned k = 0; k < 2; k++) {
        uint32_t value = type == 6 ? fbits(output[k]) : output[k];

        packed_pixel_bytes(expected + k * size, value, size, output_swap);
    }
    prepare_refused_reply(s);
    kona_command(s, 0x39, (uint32_t[]) { 64, 64, 2, 1, 12, type, 1 }, 7);
    assert_reply_posted(s);
    qtest_memread(s, RETURN_PAGE + 8, got, 2 * size);
    g_assert_cmpmem(got, 2 * size, expected, 2 * size);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_stencil_pixels_read_dma(gconstpointer opaque)
{
    unsigned type = GPOINTER_TO_UINT(opaque);
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    uint8_t expected[32], got[32];
    const uint64_t reason = 0x300000;
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current_depth(s, 24);
    stencil_config(s, 8);
    gl_window_cleared(s, 0, 0, 1);
    stencil_clear(s, 0xa5, 255);
    pixel_transfer_set(s, 0xd12, -1);
    pixel_transfer_set(s, 0xd13, 3);
    kona_command(s, 0x33, (uint32_t[]) { 0xd00, 1 }, 2);
    prepare_refused_reply(s);
    kona_command_kicked(s, 0x1f1,
        (uint32_t[]) { physical_page_word(reason), tagged_offset_word(reason),
                       0x4000 }, 3);
    qtest_writel(s, KONA_REG(0x100000), 0x20000380);
    read_dma_clear_notifications(s, reason);
    qtest_memset(s, 0x380000, 0xa5, sizeof(got));
    kona_command(s, 0x1f5, (uint32_t[]) { 0, 0x9003, 2 * size, size, 5, 0 }, 6);
    kona_command(s, 0x39, (uint32_t[]) { 64, 64, 1, 2, 12, type, 3 }, 7);
    kona_command(s, 0x1f6, (uint32_t[]) { 0x100, 0x1234 }, 2);
    memset(expected, 0xa5, sizeof(expected));
    packed_pixel_bytes(expected + 3, type == 6 ? fbits(85) : 85, size, true);
    packed_pixel_bytes(expected + 3 + size + 5,
                         type == 6 ? fbits(85) : 85, size, true);
    qtest_memread(s, 0x380000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    g_assert_cmphex(guest_word(s, reason), ==, 0x100);
    assert_reply_unchanged(s);
    qtest_quit(s);
}

static void test_read_pixels_dma(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const unsigned formats[] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 8 };
    static const uint32_t maxima[] = {
        0xff, 0xff, 0xffff, 0xffff, 0xffffffff, 0xffffffff,
    };
    bool matrix = mode >= 64;
    unsigned format = matrix ? formats[(mode - 64) / 7] : mode == 9 ? 5 : 4;
    unsigned type = matrix ? (mode - 64) % 7 :
                    mode == 8 || mode == 42 ? 3 : mode == 9 ? 6 : 1;
    unsigned components = format <= 3 || format == 6 ? 1 : format == 7 ? 2 :
                          format == 4 || format == 9 ? 3 : 4;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    unsigned row_bytes = 2 * components * size, total = 2 * row_bytes;
    bool strided = mode < 8 ? mode >= 4 : matrix ? mode & 1 : mode == 33;
    bool swap = mode == 8 || mode == 42;
    bool transferred = matrix || swap;
    bool refused = mode >= 18 && mode <= 50 && mode != 42;
    const uint64_t reason_page = 0x300000, destination = 0x380000;
    const uint32_t index = 0x13579bdf;
    uint32_t phase = mode < 8 ? mode & 3 : matrix ? mode & 3 : 3;
    uint32_t line = strided ? row_bytes : total;
    uint32_t gap = mode == 33 ? 16384 - row_bytes : strided ? 5 : 0;
    uint32_t pitch = strided ? row_bytes + gap : row_bytes;
    uint32_t start[] = { 0, 0x9000 | phase, total, line, gap, 0 };
    uint32_t read[] = { 0, 0, 2, 2, format, type, phase };
    g_autofree uint8_t *expected = g_malloc(20000);
    g_autofree uint8_t *got = g_malloc(20000);
    uint8_t reason[0x4000], actual[0x4000], hub[8];
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 1, 0, 0);
    if (mode != 45) {
        kona_command_kicked(s, 0x1f1,
                            (uint32_t[]) {
                                physical_page_word(mode == 41 ?
                                                   0x8000000 : reason_page),
                                tagged_offset_word(reason_page), 0x4000 }, 3);
    }
    qtest_writel(s, KONA_REG(0x000034), 0x98000);
    qtest_writel(s, KONA_REG(0x00003c), 0x1800090);
    qtest_writel(s, KONA_REG(0x00005c), 0x3e2210);
    qtest_writel(s, KONA_REG(0x100000), mode == 32 ? 0 : 0x20000380);
    qtest_memset(s, destination, 0xa5, 20000);
    read_dma_clear_notifications(s, reason_page);
    prepare_refused_reply(s);
    if (transferred) {
        pixel_transfer_set(s, 0xd14, 0);
        pixel_transfer_set(s, 0xd15, 0.125);
        pixel_transfer_set(s, 0xd19, 0.25);
        pixel_transfer_set(s, 0xd1b, 0.375);
        pixel_transfer_set(s, 0xd1c, 0);
        pixel_transfer_set(s, 0xd1d, 0.625);
    } else if (mode == 11) {
        pixel_transfer_set(s, 0xd14, 0.5);
    } else if (mode == 40) {
        pixel_transfer_set(s, 0xd14, INFINITY);
    }
    if (swap) {
        kona_command(s, 0x33, (uint32_t[]) { 0xd00, 1 }, 2);
    }
    switch (mode) {
    case 25:
        start[1] |= 0x10000;
        break;
    case 26:
        start[2]--;
        break;
    case 27:
        start[3] = 3;
        break;
    case 28:
        start[4] = 1;
        break;
    case 29:
        start[5] = 0x100;
        break;
    case 30:
        start[3] = row_bytes;
        start[4] = 64 * 1024 * 1024;
        break;
    case 31:
        read[6] = 0;
        break;
    case 34:
        read[4] = 11;
        break;
    case 35:
        read[5] = 7;
        break;
    case 36:
        read[2] = 8;
        read[3] = 1;
        start[2] = start[3] = 24;
        break;
    case 37:
        read[2] = 0;
        break;
    case 38:
        read[3] = 0xffffffff;
        break;
    case 39:
        read[1] = 127;
        break;
    case 47:
        start[0] = 4;
        break;
    case 46:
        return_context_deactivate(s);
        read_dma_clear_notifications(s, reason_page);
        break;
    default:
        break;
    }
    if (mode == 15) {
        kona_command(s, 0x1f5, start, 3);
        read_dma_migrate(from, &s, &to, &dir);
        for (unsigned k = 3; k < ARRAY_SIZE(start); k++) {
            qtest_writel(s, REG_COMMAND, start[k]);
        }
    } else if (mode != 23 && mode != 48) {
        kona_command(s, 0x1f5, start, ARRAY_SIZE(start));
    }
    if (mode == 14) {
        read_dma_migrate(from, &s, &to, &dir);
    } else if (mode == 18) {
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
        read_dma_clear_notifications(s, reason_page);
    }
    if (mode == 16) {
        kona_command(s, 0x39, read, 4);
        read_dma_migrate(from, &s, &to, &dir);
        for (unsigned k = 4; k < ARRAY_SIZE(read); k++) {
            qtest_writel(s, REG_COMMAND, read[k]);
        }
    } else if (mode == 43) {
        kona_command(s, 0x39, read, 4);
        qtest_writel(s, REG_COMMAND + 0xc + 0x20 * 0x1fc, 0);
        return_context_restore(s, SAVE_AREA);
        read_dma_clear_notifications(s, reason_page);
    } else if (mode == 50) {
        kona_command(s, 0x138, (uint32_t[]) { 0, 0 }, 2);
    } else if (mode != 22 && mode != 23 && mode != 48) {
        kona_command(s, 0x39, read, ARRAY_SIZE(read));
    }
    assert_reply_unchanged(s);
    memset(expected, 0xa5, 20000);
    qtest_memread(s, destination, got, 20000);
    g_assert_cmpmem(got, 20000, expected, 20000);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & 0x20, ==, 0);
    if (mode == 10) {
        gl_window_cleared(s, 0, 0, 1);
    } else if (mode == 11) {
        pixel_transfer_set(s, 0xd14, 0);
    } else if (swap) {
        kona_command(s, 0x33, (uint32_t[]) { 0xd00, 0 }, 2);
    }
    if (mode == 13 || mode == 42) {
        read_dma_migrate(from, &s, &to, &dir);
    } else if (mode == 19) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
        read_dma_clear_notifications(s, reason_page);
    } else if (mode == 20) {
        kona_command(s, 0x39, read, ARRAY_SIZE(read));
    } else if (mode == 21) {
        kona_command(s, 0x1f5, start, ARRAY_SIZE(start));
    } else if (mode == 49) {
        kona_command(s, 0x39, read, 4);
        qtest_writel(s, REG_COMMAND + 0xc + 0x20 * 0x1fc, 0);
        return_context_restore(s, SAVE_AREA);
        read_dma_clear_notifications(s, reason_page);
    } else if (mode == 48) {
        GError *err = NULL;
        g_autofree char *uri = NULL;

        dir = g_dir_make_tmp("sn0-kona-read-reuse-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_init(MACHINE_ARGS " -incoming defer");
        gl_make_current(to);
        gl_window_cleared(to, 0, 0, 1);
        kona_command(to, 0x1f5, start, ARRAY_SIZE(start));
        kona_command(to, 0x39, read, ARRAY_SIZE(read));
        qtest_qmp_assert_success(to,
            "{ 'execute': 'migrate-incoming', 'arguments': { 'uri': %s } }",
            uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (mode == 17) {
        kona_command(s, 0x1f6, (uint32_t[]) { 0x100 }, 1);
        read_dma_migrate(from, &s, &to, &dir);
        qtest_writel(s, REG_COMMAND, index);
    } else if (mode == 44) {
        kona_command(s, 0x1f6, (uint32_t[]) { 0x100 }, 1);
    } else {
        kona_command(s, 0x1f6,
                     (uint32_t[]) { mode == 24 ? 0x101 : 0x100, index }, 2);
    }
    if (!refused || mode == 41) {
        double values[4] = { transferred ? 0.125 : mode == 11 ? 0.5 : 1,
                             transferred ? 0.25 : 0,
                             transferred ? 0.375 : 0,
                             transferred ? 0.625 : 1 };
        double packed[4];

        switch (format) {
        case 0:
        case 1:
        case 2:
            packed[0] = values[format];
            break;
        case 3:
            packed[0] = values[3];
            break;
        case 6:
        case 7:
            packed[0] = values[0] + values[1] + values[2];
            packed[1] = values[3];
            break;
        case 8:
            packed[0] = values[3];
            packed[1] = values[2];
            packed[2] = values[1];
            packed[3] = values[0];
            break;
        case 9:
        case 10:
            packed[0] = values[2];
            packed[1] = values[1];
            packed[2] = values[0];
            packed[3] = values[3];
            break;
        default:
            memcpy(packed, values, sizeof(packed));
            break;
        }
        for (unsigned y = 0; y < 2; y++) {
            for (unsigned x = 0; x < 2; x++) {
                for (unsigned c = 0; c < components; c++) {
                    uint8_t *out = expected + phase + y * pitch +
                                   (x * components + c) * size;
                    double scaled = type == 6 ? 0 : packed[c] * maxima[type];
                    uint32_t bits;

                    if (type != 6 && !(type & 1)) {
                        scaled = (scaled - 1) / 2;
                    }
                    bits = type == 6 ? fbits(packed[c]) : floor(scaled + 0.5);
                    for (unsigned k = 0; k < size; k++) {
                        unsigned shift = 8 * (swap ? k : size - 1 - k);

                        out[k] = bits >> shift;
                    }
                }
            }
        }
    }
    qtest_memread(s, destination, got, 20000);
    g_assert_cmpmem(got, 20000, expected, 20000);
    memset(reason, 0xa5, sizeof(reason));
    memset(reason, 0, 4);
    if (!refused) {
        stl_be_p(reason, 0x100);
        stl_be_p(reason + 4, index);
        stl_be_p(reason + 8, 0);
        stl_be_p(reason + 12, 0);
    }
    qtest_memread(s, reason_page, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), reason, sizeof(reason));
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & 0x20, ==,
                    refused ? 0 : 0x20);
    g_assert_true(qtest_memread_cpu(s, 0, HUB_LOCAL + PI_INT_PEND0,
                                   hub, sizeof(hub)));
    g_assert_cmphex(ldq_be_p(hub) & (UINT64_C(1) << 0x11), ==,
                    refused ? 0 : UINT64_C(1) << 0x11);
    g_assert_cmphex(ldq_be_p(hub) & (UINT64_C(1) << 0x10), ==, 0);
    assert_reply_unchanged(s);
    if (mode == 12) {
        read_dma_clear_notifications(s, reason_page);
        qtest_memset(s, destination, 0xa5, 20000);
        kona_command(s, 0x1f6, (uint32_t[]) { 0x100, index }, 2);
        memset(expected, 0xa5, 20000);
        qtest_memread(s, destination, got, 20000);
        g_assert_cmpmem(got, 20000, expected, 20000);
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & 0x20, ==, 0);
        g_assert_cmphex(guest_word(s, reason_page), ==, 0);
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_read_pixels_colour(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned format = mode >= 110 ? 8 :
                      mode >= 96 ? 9 + (mode - 96) / 7 :
                      mode >= 64 ? (mode - 64) / 7 :
                      mode < 35 ? 3 + mode / 7 : 4;
    unsigned type = mode >= 110 ? (mode - 110) % 7 :
                    mode >= 96 ? (mode - 96) % 7 :
                    mode >= 64 ? (mode - 64) % 7 :
                    mode < 35 ? mode % 7 : mode == 35 ? 5 : 4;
    unsigned components = format <= 3 || format == 6 ? 1 :
                          format == 7 ? 2 :
                          (format == 4 || format == 9) ? 3 : 4;
    unsigned scalar_bytes = type < 2 ? 1 : type < 4 ? 2 : 4;
    static const uint32_t maximum[] = {
        0x7f, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0xffffffff, 0x3f800000,
    };
    static const uint32_t pixels[] = {
        0x00000000, 0xff000020, 0x00ff0040, 0x0000ff80,
    };
    uint8_t expected[0x4000], got[0x4000];
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x3a, (uint32_t[]) { 2, 2, 5, 1, 0, 0, 0, 0 }, 8);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND,
                     mode < 35 || mode >= 64 ? pixels[k] : 0x4080c080);
    }
    memset(expected, 0xa5, sizeof(expected));
    stl_be_p(expected, 1);
    for (unsigned pixel = 0; pixel < 4; pixel++) {
        for (unsigned c = 0; c < components; c++) {
            bool one = format == 8 ? (c == 0 || pixel == 4 - c) :
                       format >= 9 ? (c == 3 || pixel == 3 - c) :
                       format <= 2 ? pixel == format + 1 :
                       format == 3 || (format == 7 && c == 1) ||
                       (format == 5 && c == 3) ||
                       ((format == 6 || format == 7) ? pixel != 0 :
                                                      pixel == c + 1);
            uint32_t bits = one ? maximum[type] : 0;
            uint8_t *out = expected + 8 +
                           (pixel * components + c) * scalar_bytes;

            if (mode >= 35 && mode < 64) {
                static const uint32_t unsigned_mid[] = {
                    0x40404040, 0x80808080, 0xc0c0c0c0,
                };
                static const uint32_t signed_mid[] = {
                    0x20202020, 0x40404040, 0x60606060,
                };

                bits = type == 5 ? unsigned_mid[c] : signed_mid[c];
            }
            if (scalar_bytes == 1) {
                *out = bits;
            } else if (scalar_bytes == 2) {
                stw_be_p(out, bits);
            } else {
                stl_be_p(out, bits);
            }
        }
    }
    prepare_refused_reply(s);
    kona_command(s, 0x39,
                 (uint32_t[]) { 64, 64, 2, 2, format, type, 3 }, 7);
    qtest_memread(s, RETURN_PAGE, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    qtest_quit(s);
}

static void test_read_pixels_pack_swap(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const unsigned formats[] = { 4, 5, 7 };
    static const uint32_t maximum[] = {
        0xff, 0xff, 0xffff, 0xffff, 0xffffffff, 0xffffffff,
    };
    unsigned format = mode >= 68 ? 8 :
                      mode >= 54 ? 9 + (mode - 54) / 7 :
                      mode >= 32 ? (mode == 53 ? 1 : (mode - 32) / 7) :
                      mode < 21 ? formats[mode / 7] : mode == 30 ? 3 : 5;
    unsigned type = mode >= 68 ? (mode - 68) % 7 :
                    mode >= 54 ? (mode - 54) % 7 :
                    mode >= 32 ? (mode == 53 ? 3 : (mode - 32) % 7) :
                    mode < 21 ? mode % 7 : 3;
    unsigned components = format <= 3 ? 1 : format == 7 ? 2 :
                          (format == 4 || format == 9) ? 3 : 4;
    unsigned scalar_bytes = type < 2 ? 1 : type < 4 ? 2 : 4;
    bool swap = mode != 21 && mode != 22 && mode != 24 &&
                mode != 27 && mode != 28 && mode != 29;
    uint32_t query[] = { 0, 0, 2, 2, format, type, 3 };
    uint8_t expected[0x4000], got[0x4000];
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    if (mode != 21 && mode != 22) {
        kona_command(s, 0x33, (uint32_t[]) { 0xd00, 3 }, 2);
    }
    if (mode == 22 || mode == 23) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
    } else if (mode == 24) {
        kona_command(s, 0x33, (uint32_t[]) { 0xd00, 0 }, 2);
    } else if (mode == 26 || mode == 27) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        if (mode == 26) {
            kona_command(s, 0x33, (uint32_t[]) { 0xd00, 0 }, 2);
            return_context_deactivate(s);
            return_context_restore(s, SAVE_AREA);
        } else {
            uint32_t config[10] = { 0, 0xc };

            kona_command(s, 1, config, ARRAY_SIZE(config));
            kona_command(s, 2, (uint32_t[]) { 0 }, 1);
        }
        gl_window_cleared(s, 0, 0, 0);
    } else if (mode == 28) {
        qtest_system_reset(s);
        gl_make_current(s);
        gl_window_cleared(s, 0, 0, 0);
    } else if (mode == 29) {
        kona_command(s, 0xc9, (uint32_t[]) { 0x20 }, 1);
        kona_command(s, 0x33, (uint32_t[]) { 0xd00, 0 }, 2);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
    } else if (mode == 31) {
        gl_begin(s, 4);
        kona_command(s, 0x33, (uint32_t[]) { 0xd00, 0 }, 2);
        gl_end(s);
    }
    pixel_transfer_set(s, 0xd15, 0.125);
    pixel_transfer_set(s, 0xd19, 0.25);
    pixel_transfer_set(s, 0xd1b, 0.375);
    pixel_transfer_set(s, 0xd1c, 0);
    pixel_transfer_set(s, 0xd1d, 0.625);
    memset(expected, 0xa5, sizeof(expected));
    stl_be_p(expected, 1);
    for (unsigned pixel = 0; pixel < 4; pixel++) {
        for (unsigned c = 0; c < components; c++) {
            double value = format == 8 ? (c == 0 ? 0.625 : (4 - c) / 8.0) :
                           format >= 9 ? (c == 3 ? 0.625 : (3 - c) / 8.0) :
                           format <= 2 ? (format + 1) / 8.0 :
                           format == 3 || (format == 7 && c == 1) || c == 3 ?
                           0.625 : format == 7 ? 0.75 : (c + 1) / 8.0;
            double converted = type == 6 ? 0 : value * maximum[type];
            uint32_t bits;
            uint8_t *out = expected + 8 +
                           (pixel * components + c) * scalar_bytes;

            if (type != 6 && !(type & 1)) {
                converted = (converted - 1) / 2;
            }
            bits = type == 6 ? fbits(value) : floor(converted + 0.5);

            for (unsigned k = 0; k < scalar_bytes; k++) {
                unsigned shift = 8 * (swap ? k : scalar_bytes - 1 - k);

                out[k] = bits >> shift;
            }
        }
    }
    prepare_refused_reply(s);
    if (mode == 25 || mode == 53) {
        kona_command(s, 0x39, query, 4);
        dir = g_dir_make_tmp("sn0-kona-pack-read-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
        for (unsigned k = 4; k < ARRAY_SIZE(query); k++) {
            qtest_writel(s, REG_COMMAND, query[k]);
        }
    } else {
        kona_command(s, 0x39, query, ARRAY_SIZE(query));
    }
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    qtest_memread(s, RETURN_PAGE, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    assert_native_query_word(s, 0xb92, 0x207, false);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_read_pixels_luminance_transfer(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned type = mode == 0 ? 1 : mode == 1 ? 3 : 6;
    unsigned format = mode == 1 ? 7 : 6;
    uint8_t expected[0x4000], got[0x4000];
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    gl_window_cleared(s, mode == 3 ? 1 : 0, 0, 0);
    if (mode == 3) {
        pixel_transfer_set(s, 0xd14, FLT_MAX);
        pixel_transfer_set(s, 0xd15, FLT_MAX);
        pixel_transfer_set(s, 0xd18, 0);
        pixel_transfer_set(s, 0xd19, -FLT_MAX);
    } else {
        pixel_transfer_set(s, 0xd15, 2);
        pixel_transfer_set(s, 0xd19, -1.5);
    }
    pixel_transfer_set(s, 0xd1a, 0);
    pixel_transfer_set(s, 0xd1c, 0.5);
    memset(expected, 0xa5, sizeof(expected));
    stl_be_p(expected, 1);
    for (unsigned k = 0; k < 4; k++) {
        if (type == 1) {
            expected[8 + k] = 128;
        } else if (type == 3) {
            stw_be_p(expected + 8 + k * 4, 32768);
            stw_be_p(expected + 10 + k * 4, 32768);
        } else {
            stl_be_p(expected + 8 + k * 4,
                     mode == 3 ? 0x3f800000 : 0x3f000000);
        }
    }
    prepare_refused_reply(s);
    kona_command(s, 0x39,
                 (uint32_t[]) { 0, 0, 2, 2, format, type, 0 }, 7);
    qtest_memread(s, RETURN_PAGE, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    qtest_quit(s);
}

static void test_read_pixels_typed_extent(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t h[] = { 0, 0, mode < 2 ? 1023 : 8, 1, 5, 6, 3 };
    uint8_t expected[0x4000], got[0x4000];
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 1);
    kona_command_kicked(s, 0x1f3,
                        (uint32_t[]) { PROBE_X, PROBE_Y, 0x05670080,
                                       0x05670080, PROBE_X, PROBE_Y,
                                       1024, 1, 0, 0xd, 0 }, 11);
    kona_command(s, 0x1a, (uint32_t[]) { 0x4000 }, 1);
    if (mode == 1) {
        h[2] = 1024;
    } else if (mode == 2) {
        h[5] = 3;
    }
    prepare_refused_reply(s);
    kona_command(s, 0x39, h, ARRAY_SIZE(h));
    if (mode) {
        assert_reply_unchanged(s);
    } else {
        memset(expected, 0xa5, sizeof(expected));
        stl_be_p(expected, 1);
        for (unsigned k = 0; k < 1023; k++) {
            stl_be_p(expected + 8 + k * 16, 0);
            stl_be_p(expected + 12 + k * 16, 0);
            stl_be_p(expected + 16 + k * 16, 0x3f800000);
            stl_be_p(expected + 20 + k * 16, 0x3f800000);
        }
        qtest_memread(s, RETURN_PAGE, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    }
    assert_native_query_word(s, 0xb92, 0x207, false);
    qtest_quit(s);
}

static const struct {
    unsigned bytes;
    uint32_t bits, rgba, red_alpha, blue_alpha;
} packed_pixel_cases[] = {
    { 1, 0xa7, 0xb624ffff, 0xe0, 3 },
    { 2, 0x1357, 0x11335577, 0xf00f, 0xff },
    { 2, (17 << 11) | (9 << 6) | (3 << 1) | 1,
      0x8c4a19ff, 0xf801, 0x3f },
    { 4, 0x12345678, 0x12345678, 0xff0000ff, 0xffff },
    { 4, (513u << 22) | (257 << 12) | (129 << 2) | 2,
      0x804020aa, 0xffc00003, 0xfff },
};

static void packed_pixel_bytes(uint8_t *out, uint32_t bits, unsigned bytes,
                                 bool swap)
{
    for (unsigned k = 0; k < bytes; k++) {
        out[k] = bits >> (8 * (swap ? k : bytes - 1 - k));
    }
}

static void test_packed_pixel_ingress(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque), n = mode % 9;
    unsigned slot = n < 5 ? n : n - 4;
    unsigned type = 8 + slot, format = n >= 5 ? 10 : slot ? 5 : 4;
    bool dma = (mode / 9) & 1, swap = (mode / 9) & 2;
    unsigned bytes = packed_pixel_cases[slot].bytes;
    uint32_t pixel = packed_pixel_cases[slot].rgba;
    uint8_t source[20] = { 0 };
    uint32_t h[20];
    QTestState *s = qtest_init(MACHINE_ARGS);
    g_autofree Expected *e = g_new(Expected, 1);

    if (n >= 5) {
        pixel = (pixel & 0x00ff00ff) | ((pixel & 0xff000000) >> 16) |
                ((pixel & 0x0000ff00) << 16);
    }
    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf0, swap }, 2);
    for (unsigned k = 0; k < 4; k++) {
        packed_pixel_bytes(source + 1 + k * bytes,
                             packed_pixel_cases[slot].bits, bytes, swap);
    }
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380001, source + 1, 4 * bytes);
    }
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x3a,
                 (uint32_t[]) { 2, 1, format, type, 1, dma, 0, 0 }, 8);
    if (dma) {
        texture_dma_packet(s, 1, 2 * bytes, 2 * bytes, 0);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else {
        for (unsigned k = 0; k < DIV_ROUND_UP(1 + 2 * bytes, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(source + k * 4));
        }
    }
    expected_clear(e, 0);
    for (unsigned x = 0; x < 2; x++) {
        for (unsigned c = 0; c < 3; c++) {
            e->rgb[((127 - 64) * 128 + 64 + x) * 3 + c] =
                pixel >> (24 - c * 8);
        }
    }
    assert_window_matches(s, e);

    texture_native_bind(s, 0);
    memcpy(h, texture_header, sizeof(h));
    h[3] = format;
    h[4] = type;
    h[5] = 1;
    h[6] = dma;
    h[17] |= dma ? 4 : 0;
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    if (dma) {
        texture_dma_packet(s, 1, 4 * bytes, 4 * bytes, 0);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else {
        for (unsigned k = 0; k < DIV_ROUND_UP(1 + 4 * bytes, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(source + k * 4));
        }
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2, pixel);
    }
    qtest_quit(s);
}

static void test_packed_pixel_read(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque), n = mode % 9;
    unsigned slot = n < 5 ? n : n - 4;
    unsigned type = 8 + slot, format = n >= 5 ? 10 : slot ? 5 : 4;
    bool dma = (mode / 9) & 1, swap = (mode / 9) & 2;
    unsigned bytes = packed_pixel_cases[slot].bytes;
    const uint64_t reason_page = 0x300000, destination = 0x380000;
    uint32_t bits = n >= 5 ? packed_pixel_cases[slot].blue_alpha :
                            packed_pixel_cases[slot].red_alpha;
    uint8_t expected[0x4000], got[0x4000];
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    gl_window_cleared(s, 1, 0, 0);
    kona_command(s, 0x33, (uint32_t[]) { 0xd00, swap }, 2);
    prepare_refused_reply(s);
    if (dma) {
        kona_command_kicked(s, 0x1f1,
            (uint32_t[]) { physical_page_word(reason_page),
                           tagged_offset_word(reason_page), 0x4000 }, 3);
        qtest_writel(s, KONA_REG(0x000034), 0x98000);
        qtest_writel(s, KONA_REG(0x00003c), 0x1800090);
        qtest_writel(s, KONA_REG(0x00005c), 0x3e2210);
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        read_dma_clear_notifications(s, reason_page);
        qtest_memset(s, destination, 0xa5, sizeof(got));
        kona_command(s, 0x1f5,
            (uint32_t[]) { 0, 0x9003, 4 * bytes, 2 * bytes, 5, 0 }, 6);
    }
    kona_command(s, 0x39, (uint32_t[]) { 0, 0, 2, 2, format, type, 3 }, 7);
    memset(expected, 0xa5, sizeof(expected));
    if (dma) {
        assert_reply_unchanged(s);
        kona_command(s, 0x1f6, (uint32_t[]) { 0x100, 0x12345678 }, 2);
        for (unsigned k = 0; k < 4; k++) {
            packed_pixel_bytes(expected + 3 + (k / 2) * (2 * bytes + 5) +
                                (k % 2) * bytes, bits, bytes, swap);
        }
        qtest_memread(s, destination, got, sizeof(got));
        g_assert_cmphex(guest_word(s, reason_page), ==, 0x100);
        g_assert_cmphex(guest_word(s, reason_page + 4), ==, 0x12345678);
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & 0x20, ==, 0x20);
        assert_reply_unchanged(s);
    } else {
        stl_be_p(expected, 1);
        for (unsigned k = 0; k < 4; k++) {
            packed_pixel_bytes(expected + 8 + k * bytes, bits, bytes, swap);
        }
        qtest_memread(s, RETURN_PAGE, got, sizeof(got));
    }
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
    qtest_quit(s);
}

static void test_reordered_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool abgr = mode >= 4, swap = mode == 2 || mode == 5;
    unsigned format = abgr ? 8 : mode & 1 ? 10 : 9;
    unsigned components = abgr || (mode & 1) ? 4 : 3;
    unsigned type = mode == 2 || mode == 5 ? 3 :
                    mode == 3 || mode == 6 ? 6 : 1;
    unsigned size = type == 1 ? 1 : type == 3 ? 2 : 4;
    bool dma = mode >= 2 && mode != 4;
    const uint8_t pixels[2][4] = { { 64, 128, 192, 128 },
                                  { 16, 32, 48, 64 } };
    uint8_t source[36] = { 0 };
    uint32_t h[] = { 2, 1, format, type, 1, dma, 0, 0 };
    unsigned bytes = 2 * components * size;
    QTestState *s = qtest_init(MACHINE_ARGS);
    g_autofree Expected *e = g_new(Expected, 1);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    if (swap) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
    }
    for (unsigned x = 0; x < 2; x++) {
        for (unsigned c = 0; c < components; c++) {
            unsigned channel = abgr ? (c ? c - 1 : 3) : c;
            uint8_t *at = source + 1 + (x * components + c) * size;

            if (size == 1) {
                *at = pixels[x][channel];
            } else if (size == 2) {
                stw_le_p(at, pixels[x][channel] * 257 + 1);
            } else {
                stl_be_p(at, fbits(pixels[x][channel] / 255.0));
            }
        }
    }
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380001, source + 1, bytes);
    }
    kona_command(s, 0x3a, h, ARRAY_SIZE(h));
    if (dma) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 0 }, 2);
        texture_dma_packet(s, 1, bytes, bytes, 0);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else {
        for (unsigned k = 0; k < DIV_ROUND_UP(1 + bytes, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(source + k * 4));
        }
    }
    expected_clear(e, 0);
    for (unsigned x = 0; x < 2; x++) {
        for (unsigned c = 0; c < 3; c++) {
            e->rgb[((127 - 64) * 128 + 64 + x) * 3 + c] = pixels[x][2 - c];
        }
    }
    assert_window_matches(s, e);
    assert_native_query_word(s, 0xb92, 0x207, false);
    qtest_quit(s);
}

static void test_single_channel_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned format = mode < 42 ? (mode % 21) / 7 : mode == 45 ? 2 :
                      mode == 46 ? 0 : 1;
    unsigned type = mode < 42 ? mode % 7 : mode == 46 ? 1 : 3;
    unsigned size = type < 2 ? 1 : type < 4 ? 2 : 4;
    bool dma = (mode >= 21 && mode < 42) || mode == 43;
    bool migrate = mode == 42 || mode == 43;
    static const uint32_t maxima[] = {
        0x7f, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0xffffffff, 0x3f800000,
    };
    uint32_t raw[] = {
        !(type & 1) && type != 6 ? 1u << (size * 8 - 1) : 0,
        maxima[type],
    };
    uint32_t h[] = { 2, 1, format, type, 1, dma, 0, 0 };
    uint8_t source[12] = { 0 };
    uint32_t rgb[2][3] = { { 0 }, { 0 } };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    kona_command(s, 0xca, (uint32_t[]) { 0x204, fbits(0.5) }, 2);
    kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
    rgb[1][format] = 255;
    if (mode == 45) {
        raw[0] = 0x4000;
        raw[1] = 0xc000;
        rgb[0][2] = 64;
        rgb[1][2] = 191;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
    } else if (mode == 46) {
        pixel_transfer_set(s, 0xd14, 0.5);
        pixel_transfer_set(s, 0xd15, 0.25);
        pixel_transfer_set(s, 0xd19, 0.5);
        pixel_transfer_set(s, 0xd1b, 0.75);
        rgb[0][0] = 64;
        rgb[1][0] = 191;
        rgb[0][1] = rgb[1][1] = 128;
        rgb[0][2] = rgb[1][2] = 191;
    }
    for (unsigned x = 0; x < 2; x++) {
        for (unsigned k = 0; k < size; k++) {
            unsigned shift = 8 * (mode == 45 ? k : size - 1 - k);

            source[1 + x * size + k] = raw[x] >> shift;
        }
    }
    if (dma) {
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380001, source + 1, 2 * size);
    }
    kona_command(s, 0x3a, h, ARRAY_SIZE(h));
    if (!dma) {
        qtest_writel(s, REG_COMMAND, ldl_be_p(source));
    }
    if (migrate) {
        dir = g_dir_make_tmp("sn0-kona-single-pixels-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    } else if (mode == 44) {
        qtest_writel(s, REG_COMMAND + 0xc + 0x20 * 0x1fc, 0);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
        memset(rgb, 0, sizeof(rgb));
    }
    if (dma) {
        texture_dma_packet(s, 1, 2 * size, 2 * size, 0);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else if (mode != 44) {
        for (unsigned k = 1; k < DIV_ROUND_UP(1 + 2 * size, 4); k++) {
            qtest_writel(s, REG_COMMAND, ldl_be_p(source + k * 4));
        }
    }
    expected_clear(e, 0);
    for (unsigned x = 0; x < 2; x++) {
        for (unsigned c = 0; c < 3; c++) {
            e->rgb[((127 - 64) * 128 + 64 + x) * 3 + c] = rgb[x][c];
        }
    }
    assert_window_matches(s, e);
    assert_native_query_word(s, 0xb92, 0x207, false);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_draw_pixels_scalar_pio(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const uint32_t types[] = { 0, 2, 3, 4, 5, 6, 3, 6, 0, 6, 6, 6, 5 };
    static const uint32_t scales[] = { 0xd14, 0xd18, 0xd1a };
    static const uint32_t biases[] = { 0xd15, 0xd19, 0xd1b };
    uint32_t h[] = { 2, 1, 6, types[mode], 0, 0, 0, 0 };
    uint32_t data[8] = { 0 };
    uint32_t rgb[2] = { 0, 0xffffff };
    unsigned words = 1;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    switch (mode) {
    case 0:
        data[0] = 0x807f0000;
        break;
    case 1:
        data[0] = 0x80007fff;
        break;
    case 2:
        data[0] = 0x0000ffff;
        break;
    case 3:
        data[0] = 0x80000000;
        data[1] = 0x7fffffff;
        words = 2;
        break;
    case 4:
        data[1] = 0xffffffff;
        words = 2;
        break;
    case 5:
        data[1] = 0x3f800000;
        words = 2;
        break;
    case 6:
        data[0] = 0x004000c0;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
        rgb[0] = 0x404040;
        rgb[1] = 0xbfbfbf;
        break;
    case 7:
        data[0] = 0x0000403e;
        data[1] = 0x0000403f;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
        rgb[0] = 0x303030;
        rgb[1] = 0xbfbfbf;
        words = 2;
        break;
    case 8:
        data[0] = 0x807f0000;
        pixel_transfer_set(s, 0xd15, 0.5);
        pixel_transfer_set(s, 0xd19, 0.5);
        pixel_transfer_set(s, 0xd1b, 0.5);
        break;
    case 9:
        h[0] = 1;
        h[2] = 5;
        data[0] = 0x3fc00000;
        data[1] = 0xbf000000;
        data[2] = 0x3f000000;
        data[3] = 0x3f800000;
        for (unsigned k = 0; k < 3; k++) {
            pixel_transfer_set(s, scales[k], 0.5);
            pixel_transfer_set(s, biases[k], 0.25);
        }
        rgb[0] = 0xff0080;
        words = 4;
        break;
    case 10:
        h[0] = 1;
        h[2] = 5;
        data[0] = data[1] = data[2] = 0x3f800000;
        data[3] = 0x3f000000;
        pixel_transfer_set(s, 0xd1c, 0.999);
        kona_command(s, 0xca, (uint32_t[]) { 0x206, 0x3f000000 }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
        words = 4;
        break;
    case 11:
        h[2] = 5;
        for (unsigned k = 0; k < 8; k++) {
            data[k] = 0x3f800000;
        }
        data[7] = 0x7fc00000;
        rgb[1] = 0;
        words = 8;
        break;
    case 12:
        h[0] = 1;
        data[0] = 0x7fffffff;
        pixel_transfer_set(s, 0xd14, -4294967296.0);
        pixel_transfer_set(s, 0xd15, 2147483648.0);
        rgb[0] = 0x807f7f;
        break;
    }
    kona_command(s, 0x3a, h, ARRAY_SIZE(h));
    qtest_writel(s, REG_COMMAND, data[0]);
    if (mode == 7) {
        dir = g_dir_make_tmp("sn0-kona-scalar-pixels-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    for (unsigned k = 1; k < words; k++) {
        qtest_writel(s, REG_COMMAND, data[k]);
    }
    /* A completed or refused image must leave the next command aligned. */
    kona_command_f(s, 0x2f, (float[]) { 1, 1 }, 2);
    expected_clear(e, 0);
    for (unsigned k = 0; k < h[0]; k++) {
        unsigned at = ((127 - 64) * 128 + 64 + k) * 3;

        e->rgb[at] = rgb[k] >> 16;
        e->rgb[at + 1] = rgb[k] >> 8;
        e->rgb[at + 2] = rgb[k];
    }
    assert_window_matches(s, e);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_draw_pixels_transfer(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode == 1;
    bool migrate = mode == 2 || mode == 5 || mode == 10;
    bool refused = mode == 9 || mode == 10 || mode == 12;
    uint32_t rgb = 0x80c060;
    uint32_t h[] = { 2, 2, 5, 1, 0, dma, 0, 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    pixel_transfer_set(s, 0xd14, 2);
    pixel_transfer_set(s, 0xd19, 0.25);
    pixel_transfer_set(s, 0xd1a, 0.5);
    if (mode == 3 || mode == 4 || mode == 5 || mode == 13) {
        kona_command(s, 0xc9, (uint32_t[]) { mode == 4 ? 0x2020 : 0x20 }, 1);
        pixel_transfer_set(s, 0xd14, 0);
    } else if (mode == 6) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        pixel_transfer_set(s, 0xd14, 0);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    } else if (mode == 7 || mode == 8) {
        if (mode == 7) {
            return_context_deactivate(s);
            attrib_context_fresh(s, SAVE_AREA + 0x20000);
        } else {
            qtest_system_reset(s);
            gl_make_current(s);
        }
        gl_window_cleared(s, 0, 0, 0);
        kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
        rgb = 0x4080c0;
    } else if (mode == 11) {
        gl_begin(s, 4);
        pixel_transfer_set(s, 0xd14, 0);
        gl_end(s);
    }
    if (mode == 9 || mode == 10 || mode == 14) {
        pixel_transfer_set(s, 0xd14, INFINITY);
        if (mode == 14) {
            pixel_transfer_set(s, 0xd14, 2);
        }
    } else if (mode == 12 || mode == 13) {
        pixel_transfer_set(s, mode == 12 ? 0xffff : 0xd10, 1);
        pixel_transfer_set(s, 0xd14, 2);
    } else if (mode == 15) {
        const uint32_t irrelevant[] = { 0xd11, 0xd12, 0xd13, 0xd1e, 0xd1f };

        for (unsigned k = 0; k < ARRAY_SIZE(irrelevant); k++) {
            pixel_transfer_set(s, irrelevant[k], 42);
        }
        pixel_transfer_set(s, 0xd10, -0.0);
    } else if (mode == 16) {
        pixel_transfer_set(s, 0xd14, 4);
        pixel_transfer_set(s, 0xd19, -1);
        pixel_transfer_set(s, 0xd1b, 1);
        rgb = 0xff00ff;
    } else if (mode == 17 || mode == 18) {
        pixel_transfer_set(s, 0xd14, 1);
        pixel_transfer_set(s, 0xd19, 0);
        pixel_transfer_set(s, 0xd1a, 1);
        pixel_transfer_set(s, 0xd1c, 0);
        pixel_transfer_set(s, 0xd1d, mode == 17 ? 0.5 : 0.25);
        if (mode == 17) {
            kona_command(s, 0xca, (uint32_t[]) { 0x204, 0x3f000000 }, 2);
            kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
            refused = true;
        } else {
            kona_command(s, 0xcb, (uint32_t[]) { 0x302, 0x303 }, 2);
            kona_command(s, 0x24, (uint32_t[]) { 0xbe2 }, 1);
            rgb = 0x102030;
        }
    }
    if (mode == 2) {
        kona_command(s, 0x3a, h, ARRAY_SIZE(h));
        qtest_writel(s, REG_COMMAND, 0x4080c080);
    }
    if (migrate) {
        dir = g_dir_make_tmp("sn0-kona-transfer-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (mode == 3 || mode == 4 || mode == 5 || mode == 13) {
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
    }
    if (mode != 2) {
        kona_command(s, 0x3a, h, ARRAY_SIZE(h));
    }
    if (dma) {
        static const uint8_t bytes[] = {
            0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
            0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        };

        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, bytes, sizeof(bytes));
        texture_dma_packet(s, 0, sizeof(bytes), 16, 0);
    } else {
        for (unsigned k = mode == 2 ? 1 : 0; k < 4; k++) {
            qtest_writel(s, REG_COMMAND, 0x4080c080);
        }
    }
    expected_clear(e, 0);
    if (!refused) {
        for (unsigned k = 0; k < 4; k++) {
            unsigned at = ((127 - (64 + k / 2)) * 128 + 64 + k % 2) * 3;

            e->rgb[at] = rgb >> 16;
            e->rgb[at + 1] = rgb >> 8;
            e->rgb[at + 2] = rgb;
        }
    }
    assert_window_matches(s, e);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_texture_pio_layout(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t h[20], raw[18] = {
        0xaa4080c0, 0x102030c0, 0x4080ffff, 0xffaaaaaa,
    };
    uint32_t expected[4] = {
        0x4080c0ff, 0x102030ff, 0xc04080ff, 0xffffffff,
    };
    unsigned count = 4, first = 1;
    bool migrate = mode == 5 || mode == 6 || mode == 10;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    texture_native_bind(s, 0);
    memcpy(h, texture_header, sizeof(h));
    h[3] = 4;
    h[5] = 1;
    kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    if (mode == 1) {
        h[1] = 1;
        h[13] = 0;
        h[5] = 0;
        kona_command(s, 0x1d5,
                     (uint32_t[]) { 0, (2u << 28) | (41 << 12),
                                    0x30 << 14, 0x10, 1 }, 5);
        kona_command(s, 0x179,
                     (uint32_t[22]) { 0x43, 0x101010, 256u << 10 }, 22);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 4 }, 2);
        raw[0] = 0x4080c0aa;
        raw[1] = 0x102030bb;
        count = 2;
    } else if (mode == 2) {
        h[7] = 1;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
        raw[0] = 0xaa4080c0;
        raw[1] = 0x102030aa;
        raw[2] = 0xaaaac040;
        raw[3] = 0x80ffffff;
    } else if (mode == 3 || mode == 5) {
        h[5] = 0;
        h[7] = 1;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 1 }, 2);
        raw[0] = 0x4080c010;
        raw[1] = 0x2030aaaa;
        raw[2] = 0xaac04080;
        raw[3] = 0xffffffaa;
    } else if (mode == 4) {
        h[5] = 3;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
    } else if (mode == 6) {
        first = 2;
    } else if (mode == 7) {
        h[15] = 1; /* Known source span with an unsupported 2D destination. */
    } else if (mode == 8) {
        h[7] = 1;
        count = first = 0;
    } else if (mode >= 9) {
        static const uint32_t floats[] = {
            0x3f800000, 0, 0, 0x3f800000,
            0, 0x3f800000, 0, 0x3f800000,
            0, 0, 0x3f800000, 0x3f800000,
            0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
        };
        uint8_t bytes[72];

        h[3] = 5;
        h[4] = 6;
        h[5] = mode == 11 ? 1 : 3;
        h[7] = mode == 11;
        expected[0] = 0xff0000ff;
        expected[1] = 0x00ff00ff;
        expected[2] = 0x0000ffff;
        expected[3] = 0xffffffff;
        memset(bytes, 0xaa, sizeof(bytes));
        if (mode == 11) {
            kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 3 }, 2);
            kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
            kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 1 }, 2);
        }
        for (unsigned k = 0; k < ARRAY_SIZE(floats); k++) {
            unsigned at = mode == 11 ? (k / 8) * 36 + 1 + (k % 8) * 4 :
                                      3 + k * 4;

            stl_be_p(bytes + at, floats[k]);
        }
        count = mode == 11 ? 18 : 17;
        for (unsigned k = 0; k < count; k++) {
            raw[k] = ldl_be_p(bytes + k * 4);
        }
        if (mode == 10) {
            first = 8;
        }
    }
    prepare_refused_reply(s);
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    for (unsigned k = 0; k < first; k++) {
        qtest_writel(s, REG_COMMAND, raw[k]);
    }
    if (migrate) {
        dir = g_dir_make_tmp("sn0-kona-texture-layout-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (count) {
        for (unsigned k = first; k + 1 < count; k++) {
            qtest_writel(s, REG_COMMAND, raw[k]);
        }
        assert_texture_staging(s, NULL, 0, count - 1);
        assert_reply_unchanged(s);
        qtest_writeq(s, REG_COMMAND, ((uint64_t)raw[count - 1] << 32) | 0x176);
        qtest_writel(s, REG_COMMAND, 0);
    } else {
        kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    }
    assert_texture_staging(s, NULL, 0, 0);
    assert_reply_unchanged(s);
    for (unsigned k = 0; k < h[1] * h[2]; k++) {
        assert_texture_pixel(s, k % h[1], k / h[1],
                             mode == 7 || mode == 8 ? 0 : expected[k]);
    }
    assert_native_query_word(s, 0xb92, 0x207, false);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_texture_pixel_transfer(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode == 1 || mode == 3 || mode == 11;
    bool migrate = mode == 2 || mode == 3 || mode == 4 || mode == 13;
    uint32_t h[20], raw[16], count = 4, expected = 0x80c06080;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    texture_native_bind(s, 0);
    kona_command(s, 0x174, texture_header, 20);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, texture_source[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    memcpy(h, texture_header, sizeof(h));
    h[6] = dma;
    h[7] = 0;
    h[17] |= dma ? 4 : 0;
    for (unsigned k = 0; k < 4; k++) {
        raw[k] = 0x4080c080;
    }
    pixel_transfer_set(s, 0xd14, 2);
    pixel_transfer_set(s, 0xd19, 0.25);
    pixel_transfer_set(s, 0xd1a, 0.5);
    if (mode == 5) {
        h[4] = 0;
        for (unsigned k = 0; k < 4; k++) {
            raw[k] = 0x8080807f;
        }
        pixel_transfer_set(s, 0xd14, 1);
        pixel_transfer_set(s, 0xd15, 0.5);
        pixel_transfer_set(s, 0xd19, 0.5);
        pixel_transfer_set(s, 0xd1a, 1);
        pixel_transfer_set(s, 0xd1b, 0.5);
        expected = 0x000000ff;
    } else if (mode == 6 || mode == 7) {
        h[4] = 6;
        count = 16;
        for (unsigned k = 0; k < 4; k++) {
            raw[k * 4] = 0x3fc00000;
            raw[k * 4 + 1] = 0xbf000000;
            raw[k * 4 + 2] = 0x3f000000;
            raw[k * 4 + 3] = 0x3f800000;
        }
        if (mode == 7) {
            raw[15] = 0x7fc00000;
        }
        pixel_transfer_set(s, 0xd14, 0.5);
        pixel_transfer_set(s, 0xd15, 0.25);
        pixel_transfer_set(s, 0xd1a, 0.5);
        pixel_transfer_set(s, 0xd1b, 0.25);
        expected = 0xff0080ff;
    } else if (mode >= 8 && mode <= 10) {
        h[9] = mode == 8 ? 0xe : mode == 9 ? 0xd : 8;
        h[17] = 0x2200;
        pixel_transfer_set(s, 0xd1c, 0.5);
        pixel_transfer_set(s, 0xd1d, 0.25);
        expected = mode == 8 ? 0x00000080 :
                   mode == 9 ? 0x800000ff : 0x80000080;
    } else if (mode == 12) {
        h[4] = 3;
        count = 8;
        for (unsigned k = 0; k < 4; k++) {
            raw[k * 2] = 0x00400080;
            raw[k * 2 + 1] = 0x00c00080;
        }
        kona_command(s, 0x33, (uint32_t[]) { 0xcf0, 1 }, 2);
        expected = 0x80bf6080;
    }
    if (mode == 13) {
        pixel_transfer_set(s, 0xd12, NAN);
    }
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    if (!dma) {
        unsigned first = mode == 4 ? count : 1;

        for (unsigned k = 0; k < first; k++) {
            qtest_writel(s, REG_COMMAND, raw[k]);
        }
    }
    if (migrate) {
        dir = g_dir_make_tmp("sn0-kona-texture-transfer-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (dma) {
        uint8_t bytes[64];

        for (unsigned k = 0; k < count; k++) {
            stl_be_p(bytes + k * 4, raw[k]);
        }
        if (mode == 3 || mode == 11) {
            pixel_transfer_set(s, 0xd14, 0);
        }
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        qtest_memwrite(s, 0x380000, bytes, count * 4);
        texture_dma_packet(s, 0, count * 4, count * 4, 0);
    } else if (mode != 4) {
        for (unsigned k = 1; k < count; k++) {
            qtest_writel(s, REG_COMMAND, raw[k]);
        }
    }
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2, texture_source[k]);
    }
    pixel_transfer_set(s, 0xd14, 0);
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    if (mode >= 8 && mode <= 10) {
        uint32_t record[22] = { 0x44, 0x101010, 256u << 10 };
        unsigned index = mode == 8 ? 17 : mode == 9 ? 0 : 1;

        kona_command(s, 0x1d5,
                     (uint32_t[]) { 0, (2u << 28) | (index << 12),
                                    h[9] << 14, 0x11, 1 }, 5);
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
    }
    for (unsigned k = 0; k < 4; k++) {
        assert_texture_pixel(s, k % 2, k / 2,
                             mode == 7 ? texture_source[k] : expected);
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_native_image_state_refusal(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t h[20];

    gl_make_current(s);
    gl_rect_scene(s);
    if (mode < 2) {
        pixel_transfer_set(s, mode == 1 ? 0xffff : 0xd14,
                           mode == 1 ? 1 : INFINITY);
    } else {
        kona_command(s, 0x33,
                     (uint32_t[]) { mode == 2 ? 0xcf3 : 0xcf4, 1 }, 2);
    }
    prepare_refused_reply(s);
    if (mode < 2) {
        kona_command(s, 0x39, (uint32_t[]) { 10, 20, 3, 2, 4, 1, 0 }, 7);
    } else {
        kona_command(s, 0x3a, (uint32_t[]) { 2, 2, 5, 1, 0, 1, 1, 0 }, 8);
    }
    assert_reply_unchanged(s);
    memcpy(h, texture_header, sizeof(h));
    if (mode >= 2) {
        h[6] = h[7] = 1;
        h[17] |= 4;
    }
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    if (mode < 2) {
        for (unsigned k = 0; k < 4; k++) {
            qtest_writel(s, REG_COMMAND, 0xffffffff);
        }
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(s, 0, 0, 0);
    kona_command(s, 0x4c, (uint32_t[]) { 0xba2 }, 1);
    assert_reply_posted(s);
    qtest_quit(s);
}

static void test_raw_bitmap(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool skips = mode == 2 || mode == 3 || mode == 4;
    bool migrate = mode == 4 || mode == 5;
    bool refused = mode == 6 || mode == 8 || mode == 9 ||
                   mode == 11;
    uint32_t h[] = { skips ? 8 : 9, 2, skips ? 1 : 0, 0, 1,
                     fbits(0), fbits(0), fbits(3), fbits(4) };
    uint32_t words[] = { 0x81800000, 0x42000000 };
    const uint8_t bits[] = { 0x81, 0x80, 0x42, 0 };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    if (mode == 12 || mode == 13) {
        const uint32_t oversized[] = { 8193, 1, 0, 0, 0, 0, 0, 0, 0 };

        kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
        kona_command(s, 0x6, oversized, ARRAY_SIZE(oversized));
        qtest_writel(s, REG_COMMAND, 0xffffffff);
        if (mode == 13) {
            dir = g_dir_make_tmp("sn0-kona-raw-drain-XXXXXX", &err);
            g_assert_no_error(err);
            uri = g_strdup_printf("unix:%s/migrate.sock", dir);
            to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
            sgi_migrate_stopped(from, uri);
            s = to;
        }
        for (unsigned k = 1; k < 257; k++) {
            qtest_writel(s, REG_COMMAND, 0xffffffff);
        }
        kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 4 }, 2);
    }
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0x63, (float[]) { 1, 0, 0 }, 3);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (!skips && mode != 1) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 16 }, 2);
    }
    if (mode == 1) {
        h[0] = 8;
        h[4] = 0;
        words[0] = 0x81420000;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
    } else if (skips) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 24 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 9 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
        words[0] = mode == 3 ? 0xaa0201bb : 0xaa4080bb;
        words[1] = mode == 3 ? 0x8400bbbb : 0x2100bbbb;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf1, mode == 3 }, 2);
    } else if (mode == 6) {
        gl_begin(s, 4);
    } else if (mode == 7) {
        kona_command(s, 0x24, (uint32_t[]) { 0xde0 }, 1);
    } else if (mode == 11) {
        kona_command_f(s, 0xa9, (float[]) { 2, 0 }, 2);
    } else if (mode == 10) {
        pixel_transfer_set(s, 0xd14, 0);
        kona_command_f(s, 0x2f, (float[]) { -2, 3 }, 2);
    }
    kona_command(s, 0x6, h, ARRAY_SIZE(h));
    qtest_writel(s, REG_COMMAND, words[0]);
    if (mode == 8) {
        qtest_writel(s, REG_COMMAND + 0xc + 0x20 * 0x1fc, 0);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    } else if (mode == 9) {
        qtest_writel(s, REG_COMMAND + 0xc + 0x20 * 0x1fc, 0);
        return_context_restore(s, SAVE_AREA);
    }
    if (migrate) {
        dir = g_dir_make_tmp("sn0-kona-raw-bitmap-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (mode != 1 && mode != 9) {
        qtest_writel(s, REG_COMMAND, words[1]);
    }
    if (mode == 6) {
        gl_end(s);
    } else if (mode == 7) {
        kona_command(s, 0x25, (uint32_t[]) { 0xde0 }, 1);
    }
    expected_clear(e, 0);
    if (!refused) {
        for (unsigned y = 0; y < 2; y++) {
            for (unsigned x = 0; x < h[0]; x++) {
                if (bits[y * 2 + x / 8] & (0x80 >> (x & 7))) {
                    unsigned at = ((127 - (64 + y)) * 128 + 64 + x) * 3;

                    e->rgb[at] = 255;
                }
            }
        }
    }
    assert_window_matches(s, e);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_raw_bitmap_dma(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool refused = (mode >= 4 && mode < 16) || mode >= 23;
    bool extended = mode >= 16;
    uint32_t h[] = { 32, 2, 0, 1, 0, 0, 0, fbits(3), fbits(4) };
    uint8_t bytes[12] = { 0x82, 0, 0, 0, 0x40, 0, 0, 0 };
    uint32_t total = 8, line = total, gap = 0;
    uint64_t io = mode == 6 ? 1 : 0;
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0x63, (float[]) { 1, 0, 0 }, 3);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    if (mode == 1) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf1, 1 }, 2);
    } else if (mode == 2) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 3 }, 2);
    } else if (mode == 4) {
        total = line = 4;
    } else if (mode == 5) {
        gap = 4;
    } else if (mode == 7) {
        h[0] = 64;
        h[1] = 2049;
        total = line = 16392;
    } else if (mode == 10) {
        h[5] = 0x7f800000;
    } else if (mode == 11) {
        line = 4;
    } else if (mode == 12) {
        h[0] = 16;
    } else if (mode == 13) {
        h[4] = 1;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 64 }, 2);
    } else if (mode == 14) {
        h[2] = 1;
    } else if (mode == 15) {
        h[4] = 1;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 1 }, 2);
    }
    if (extended) {
        memset(bytes, 0xaa, sizeof(bytes));
        h[0] = 9;
        if (mode == 16) {
            h[2] = 1;
            bytes[1] = 0x81;
            bytes[2] = 0x80;
            bytes[5] = 0x42;
            bytes[6] = 0;
        } else if (mode == 17) {
            total = line = 4;
            kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
            bytes[0] = 0x81;
            bytes[1] = 0x80;
            bytes[2] = 0x42;
            bytes[3] = 0;
        } else {
            h[4] = 1;
            total = 4;
            line = 2;
            gap = 2;
            if (mode == 18 || mode == 22) {
                h[2] = mode == 22 ? 2 : 1;
                io = mode == 22 ? 3 : 1;
                kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 32 }, 2);
                if (mode == 22) {
                    h[0] = 8;
                    kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 9 }, 2);
                }
                bytes[0] = mode == 22 ? 0x40 : 0x81;
                bytes[1] = 0x80;
                bytes[4] = mode == 22 ? 0x21 : 0x42;
                bytes[5] = 0;
            } else {
                h[0] = 8;
                gap = 1;
                kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 24 }, 2);
                kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
                kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 9 }, 2);
                kona_command(s, 0x33, (uint32_t[]) { 0xcf5, 1 }, 2);
                kona_command(s, 0x33, (uint32_t[]) { 0xcf1, mode == 20 }, 2);
                bytes[0] = mode == 20 ? 0x02 : 0x40;
                bytes[1] = mode == 20 ? 0x01 : 0x80;
                bytes[3] = mode == 20 ? 0x84 : 0x21;
                bytes[4] = 0;
            }
        }
    }
    if (mode >= 23) {
        h[0] = 9;
        h[2] = 1;
        h[4] = 1;
        total = 4;
        line = 2;
        gap = 2;
        io = mode == 23 ? 0 : 1;
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 32 }, 2);
        if (mode == 24) {
            total = 6;
            line = 3;
            gap = 1;
        } else if (mode == 25) {
            kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 131072 }, 2);
            gap = 16382;
        }
    }
    qtest_writel(s, KONA_REG(0x100000), mode == 9 ? 0 : 0x20000380);
    qtest_memwrite(s, 0x380000 + io, bytes, sizeof(bytes));
    if (mode == 7) {
        qtest_memset(s, 0x380000, 0x80, 16384);
    }
    kona_command(s, 0x6, h, ARRAY_SIZE(h));
    if (mode == 8) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
    }
    if (mode == 3 || mode == 21) {
        dir = g_dir_make_tmp("sn0-kona-bitmap-dma-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (mode == 0 || mode == 5) {
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX,
                        ==, 0);
    }
    if (mode < 12 || extended) {
        texture_dma_packet(s, io, total, line, gap);
    }
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, refused ? 0 : 0x100);
    if (mode == 0 || mode == 5) {
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX,
                        ==, refused ? 0 : INT_CAUSE_MAILBOX);
    }
    expected_clear(e, 0);
    if (!refused) {
        for (unsigned y = 0; y < 2; y++) {
            for (unsigned x = 0; x < (extended ? h[0] : 8); x++) {
                uint8_t bits = extended ?
                               (x == 8 ? (y ? 0 : 0x80) :
                                         (y ? 0x42 : 0x81)) : bytes[y * 4];

                if (bits & (mode == 1 ? 1 << x : 0x80 >> (x & 7))) {
                    e->rgb[((127 - (64 + y)) * 128 + 64 + x) * 3] = 255;
                }
            }
        }
    }
    assert_window_matches(s, e);
    /* A failed source must neither move the raster nor leave PIO pending. */
    kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 0 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 0 }, 2);
    kona_command(s, 0x3a, (uint32_t[]) { 1, 1, 5, 1, 0, 0, 0, 0 }, 8);
    qtest_writel(s, REG_COMMAND, 0x00ff00ff);
    e->rgb[((127 - (refused ? 64 : 68)) * 128 +
            (refused ? 64 : 67)) * 3 + 1] = 255;
    assert_window_matches(s, e);
    assert_native_query_word(s, 0xb92, 0x207, false);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_raw_bitmap_large(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t h[] = { 8192, 4, 0, 0, 1, 0, 0, 0, 0 };
    g_autofree Expected *e = g_new(Expected, 1);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0x63, (float[]) { 1, 0, 0 }, 3);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 8193 }, 2);
    kona_command(s, 0x6, h, ARRAY_SIZE(h));
    for (unsigned k = 0; k < 1024; k++) {
        qtest_writel(s, REG_COMMAND, k ? 0 : 0x80000000);
    }
    expected_clear(e, 0);
    e->rgb[((127 - 64) * 128 + 64) * 3] = 255;
    assert_window_matches(s, e);
    qtest_quit(s);
}

static void test_copy_pixels(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;
    static const uint32_t source[] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    uint32_t copy[] = { 64, 64, 2, 2, 0, 0x1800, 0x80000000 };
    uint32_t colours[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
    unsigned dx = mode == 1 ? 65 : 68, dy = mode == 1 ? 64 : 68;
    bool refused = mode >= 4 && mode <= 14 && mode != 12 && mode != 9;
    g_autofree Expected *e = g_new(Expected, 1);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x3a, (uint32_t[]) { 2, 2, 5, 1, 0, 0, 0, 0 }, 8);
    for (unsigned k = 0; k < ARRAY_SIZE(source); k++) {
        qtest_writel(s, REG_COMMAND, source[k]);
    }
    kona_command_f(s, 0xa9,
                   (float[]) { (dx - 64) / 64.0, (dy - 64) / 64.0 }, 2);
    if (mode == 2) {
        pixel_transfer_set(s, 0xd14, 0.5);
        colours[0] = 0x800000;
        colours[3] = 0x80ffff;
    } else if (mode == 3) {
        copy[6] = 0;
    } else if (mode == 4) {
        copy[6] = 0x80000001;
    } else if (mode == 5) {
        copy[4] = 1;
    } else if (mode == 6) {
        copy[5] = 0x1801;
    } else if (mode == 7) {
        kona_command_f(s, 0x2f, (float[]) { 2, 1 }, 2);
    } else if (mode == 8) {
        gl_begin(s, 4);
    } else if (mode == 9) {
        pixel_transfer_set(s, 0xd10, 1);
        memset(colours, 0, sizeof(colours));
    } else if (mode == 10) {
        copy[0] = 127;
    } else if (mode == 11) {
        kona_command_f(s, 0xa9, (float[]) { 2, 0 }, 2);
    } else if (mode == 12) {
        kona_command(s, 0x24, (uint32_t[]) { 0xde0 }, 1);
    } else if (mode == 13) {
        return_context_deactivate(s);
    } else if (mode == 14) {
        pixel_transfer_set(s, 0xd14, INFINITY);
    }
    if (mode == 15) {
        kona_command(s, 0x38, copy, 4);
        dir = g_dir_make_tmp("sn0-kona-copy-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
        for (unsigned k = 4; k < ARRAY_SIZE(copy); k++) {
            qtest_writel(s, REG_COMMAND, copy[k]);
        }
    } else {
        kona_command(s, 0x38, copy, ARRAY_SIZE(copy));
    }
    if (mode == 16) {
        kona_command(s, 0x3a, (uint32_t[]) { 2, 2, 5, 1, 0, 0, 0, 0 }, 8);
        for (unsigned k = 0; k < 4; k++) {
            qtest_writel(s, REG_COMMAND, 0x00ffffff);
            colours[k] = 0x00ffff;
        }
    }
    if (mode == 8) {
        gl_end(s);
    } else if (mode == 13) {
        return_context_restore(s, SAVE_AREA);
    }
    expected_clear(e, 0);
    for (unsigned k = 0; k < 4; k++) {
        unsigned at = ((127 - (64 + k / 2)) * 128 + 64 + k % 2) * 3;

        e->rgb[at] = source[k] >> 24;
        e->rgb[at + 1] = source[k] >> 16;
        e->rgb[at + 2] = source[k] >> 8;
    }
    if (!refused) {
        for (unsigned k = 0; k < 4; k++) {
            unsigned at = ((127 - (dy + k / 2)) * 128 + dx + k % 2) * 3;

            e->rgb[at] = colours[k] >> 16;
            e->rgb[at + 1] = colours[k] >> 8;
            e->rgb[at + 2] = colours[k];
        }
    }
    assert_window_matches(s, e);
    prepare_refused_reply(s);
    kona_command(s, 0x4c, (uint32_t[]) { 0xba2 }, 1);
    assert_reply_posted(s);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_copy_pixels_indexed_source(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { -1, -1 }, 2);
    kona_command(s, 0x3a, (uint32_t[]) { 3, 2, 5, 1, 0, 0, 0, 0 }, 8);
    for (unsigned k = 0; k < 6; k++) {
        qtest_writel(s, REG_COMMAND, 0xff0000ff);
    }
    x_read_fixture_command(s, 0x12e,
                            (uint32_t[]) { 0x2d07, 1, 0x304c1304 }, 3);
    x_read_fixture_command(s, 0x129,
                            (uint32_t[]) { 0, 0, 0, 0, 0xf00fff, 0,
                                           0x180c, 0x40001, 1, 6, 5, 1, 1 },
                            13);
    x_read_fixture_command(s, 0x136,
                            (uint32_t[]) { 0, 0, 1, 8, 1, 1, 0 }, 7);
    x_read_fixture_command(s, 0x139,
                            (uint32_t[]) { PROBE_X + 2, 1023 - PROBE_Y - 1 },
                            2);
    qtest_writel(s, KONA_REG(0xc1000c), 0x07000000);
    x_read_fixture_command(s, 0x13a, (uint32_t[]) { 0 }, 1);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    prepare_refused_reply(s);
    kona_command(s, 0x110, (uint32_t[]) { 0, 0, 3, 2 }, 4);
    assert_reply_unchanged(s);
    kona_command(s, 0x38,
                 (uint32_t[]) { 0, 0, 3, 2, 0, 0x1800, 0x80000000 }, 7);
    /* The last source's indexed tag must refuse the earlier red pixels too. */
    assert_stencil_colour(s, 0);
    qtest_quit(s);
}

static void test_copy_pixels_query(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *dir = NULL, *uri = NULL;
    g_autofree Expected *e = g_new(Expected, 1);
    GError *err = NULL;
    const uint32_t source[] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    uint32_t colours[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
    uint32_t query[] = { 64, 64, 2, 2 };
    uint32_t selector = mode == 9 ? 0x115 : 0x110;
    bool refused = mode >= 4 && mode <= 16 && mode != 8 && mode != 7;
    float zx = mode == 1 ? -2 : 2, zy = mode == 1 ? -1 : 3;
    int dx = mode == 2 ? 65 : 68, dy = mode == 2 ? 64 : 68;

    if (mode == 11) {
        uint32_t config[10] = { 0, 12 };

        kona_command_kicked(s, 0x1fd,
                            (uint32_t[]) { physical_page_word(SAVE_AREA),
                                           tagged_offset_word(SAVE_AREA),
                                           0x10000 }, 3);
        kona_command(s, 1, config, ARRAY_SIZE(config));
        kona_command(s, 2, (uint32_t[]) { 0 }, 1);
    } else {
        gl_make_current(s);
    }
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(s, 0x3a, (uint32_t[]) { 2, 2, 5, 1, 0, 0, 0, 0 }, 8);
    for (unsigned k = 0; k < ARRAY_SIZE(source); k++) {
        qtest_writel(s, REG_COMMAND, source[k]);
    }
    kona_command_f(s, 0xa9,
                   (float[]) { (dx - 64) / 64.0, (dy - 64) / 64.0 }, 2);
    kona_command_f(s, 0x2f, (float[]) { zx, zy }, 2);
    if (mode == 3) {
        pixel_transfer_set(s, 0xd14, 0.5);
        colours[0] = 0x800000;
        colours[3] = 0x80ffff;
    } else if (mode == 4) {
        query[0] = 127;
    } else if (mode == 5) {
        kona_command_f(s, 0x2f, (float[]) { INFINITY, 1 }, 2);
    } else if (mode == 6) {
        pixel_transfer_set(s, 0xd14, INFINITY);
    } else if (mode == 7) {
        pixel_transfer_set(s, 0xd10, 1);
        memset(colours, 0, sizeof(colours));
    } else if (mode == 8) {
        kona_command(s, 0x24, (uint32_t[]) { 0xde0 }, 1);
    } else if (mode == 10) {
        return_context_deactivate(s);
    } else if (mode == 12) {
        kona_command(s, 0x114, (uint32_t[]) { 0xabcdef }, 1);
    } else if (mode == 13) {
        kona_command(s, 0x37, (uint32_t[]) { 0x405 }, 1);
    } else if (mode == 14) {
        query[2] = UINT32_MAX;
    } else if (mode == 15) {
        query[0] = UINT32_MAX;
    } else if (mode == 16) {
        query[2] = 0;
    }
    prepare_refused_reply(s);
    if (mode == 17) {
        kona_command(s, selector, query, 2);
        dir = g_dir_make_tmp("sn0-kona-copy-query-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
        qtest_writel(s, REG_COMMAND, query[2]);
        qtest_writel(s, REG_COMMAND, query[3]);
    } else {
        kona_command(s, selector, query, ARRAY_SIZE(query));
    }
    if (refused) {
        assert_reply_unchanged(s);
    } else {
        assert_reply_posted(s);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 0);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 4), ==, 0xa5a5a5a5);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 12), ==, 0xa5a5a5a5);
        qtest_memset(s, RETURN_PAGE, 0, 4);
        kona_command(s, 0x38,
                     (uint32_t[]) { query[0], query[1], query[2], query[3],
                                    0, 0x1800, guest_word(s, RETURN_PAGE + 8) },
                     7);
    }
    expected_clear(e, 0);
    for (unsigned k = 0; k < 4; k++) {
        unsigned at = ((127 - (64 + k / 2)) * 128 + 64 + k % 2) * 3;

        e->rgb[at] = source[k] >> 24;
        e->rgb[at + 1] = source[k] >> 16;
        e->rgb[at + 2] = source[k] >> 8;
    }
    if (!refused) {
        for (unsigned k = 0; k < 4; k++) {
            int x0 = dx + (k % 2) * zx, x1 = x0 + zx;
            int y0 = dy + (k / 2) * zy, y1 = y0 + zy;

            for (int y = MIN(y0, y1); y < MAX(y0, y1); y++) {
                for (int x = MIN(x0, x1); x < MAX(x0, x1); x++) {
                    unsigned at = ((127 - y) * 128 + x) * 3;

                    e->rgb[at] = colours[k] >> 16;
                    e->rgb[at + 1] = colours[k] >> 8;
                    e->rgb[at + 2] = colours[k];
                }
            }
        }
    }
    assert_window_matches(s, e);
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (dir) {
        g_rmdir(dir);
    }
}

static void test_copy_query_context_pages(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    const uint64_t save_b = SAVE_AREA + 0x20000;
    const uint64_t page_b = RETURN_PAGE + 0x20000;
    uint32_t config[10] = { 0, 12 };
    const uint32_t query[] = { 64, 64, 2, 2 };

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0x2f, (float[]) { 2, 3 }, 2);
    return_context_deactivate(s);
    attrib_context_fresh(s, save_b);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]) { physical_page_word(page_b),
                                       tagged_offset_word(page_b), 0x4000 }, 3);
    kona_command(s, 1, config, ARRAY_SIZE(config));
    kona_command(s, 2, (uint32_t[]) { 0 }, 1);
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0x2f, (float[]) { -1, 2 }, 2);
    prepare_refused_reply(s);
    qtest_memset(s, page_b, 0xa5, 0x4000);
    qtest_memset(s, page_b, 0, 4);
    kona_command(s, 0x110, query, ARRAY_SIZE(query));
    assert_reply_unchanged(s);
    g_assert_cmphex(guest_word(s, page_b), ==, 1);
    g_assert_cmphex(guest_word(s, page_b + 8), ==, 0);
    g_assert_cmphex(guest_word(s, page_b + 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(s, page_b + 12), ==, 0xa5a5a5a5);
    qtest_memset(s, page_b, 0xa5, 0x4000);
    qtest_memset(s, page_b, 0, 4);
    return_context_deactivate(s);
    return_context_restore(s, SAVE_AREA);
    kona_command(s, 0x110, query, ARRAY_SIZE(query));
    assert_reply_posted(s);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 0);
    g_assert_cmphex(guest_word(s, page_b), ==, 0);
    g_assert_cmphex(guest_word(s, page_b + 8), ==, 0xa5a5a5a5);
    qtest_quit(s);
}

static void bitmap_expected(QTestState *s, bool bitmap, bool pixels,
                              int bx, int by, unsigned px, unsigned py,
                              bool padded)
{
    static const uint8_t bits[] = { 0x81, 0x42, 0x24, 0x18, 0, 0, 0, 0x57 };
    static const uint8_t short_bits[] = { 0x81, 0x42, 0x24, 0x18, 0x80, 0x01 };
    const uint8_t *image = padded ? short_bits : bits;
    unsigned rows = padded ? ARRAY_SIZE(short_bits) : ARRAY_SIZE(bits);
    static const uint32_t rgb[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
    g_autofree Expected *e = g_new(Expected, 1);

    expected_clear(e, 0x000000);
    if (bitmap) {
        for (unsigned row = 0; row < rows; row++) {
            for (unsigned col = 0; col < 8; col++) {
                int x = bx + col, y = by + row;

                if ((image[row] & (0x80 >> col)) &&
                    x >= 0 && x < 128 && y >= 0 && y < 128) {
                    e->rgb[((127 - y) * 128 + x) * 3] = 255;
                }
            }
        }
    }
    if (pixels) {
        for (unsigned k = 0; k < 4; k++) {
            unsigned at = ((127 - (py + k / 2)) * 128 + px + k % 2) * 3;

            e->rgb[at] = rgb[k] >> 16;
            e->rgb[at + 1] = rgb[k] >> 8;
            e->rgb[at + 2] = rgb[k];
        }
    }
    assert_window_matches(s, e);
}

static void test_native_bitmap(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool migration = mode == 3 || mode == 19;
    bool invalid = mode == 4 || mode == 6;
    bool refused = mode == 7 || mode == 8 || mode == 10 ||
                   mode == 11 || mode == 16 || mode == 18 || mode == 19;
    uint32_t h[] = { 8, 8, fbits(0.5), fbits(1.5), fbits(3.5),
                    fbits(5.5), 3, 8 };
    uint32_t move[] = { 0, 0, 0, 0, 0, 0, 0, fbits(5), fbits(6) };
    QTestState *from = qtest_init(MACHINE_ARGS), *s = from, *to = NULL;
    g_autofree char *tmpfs = NULL, *uri = NULL;
    GError *err = NULL;
    int bx = 63, by = 62;
    unsigned px = 67, py = 68;

    if (mode == 13) {
        gl_make_current_depth(s, 24);
    } else {
        gl_make_current(s);
    }
    gl_window_cleared(s, 0, 0, 0);
    kona_command_f(s, 0x63, (float[]) { 1, 0, 0 }, 3);
    kona_command_f(s, 0xad, (float[]) { 0, 0, mode == 13 ? 0.5 : 0 }, 3);
    if (mode == 1) {
        kona_command_f(s, 0x2f, (float[]) { -2, 3 }, 2);
    } else if (mode == 2) {
        kona_command(s, 0x33, (uint32_t[]) { 0x0cf1, 1 }, 2);
    } else if (invalid) {
        kona_command_f(s, 0xa9, (float[]) { 2, 0 }, 2);
    } else if (mode == 7) {
        gl_begin(s, 4);
    } else if (mode == 8) {
        return_context_deactivate(s);
    } else if (mode == 9 || mode == 15) {
        kona_command(s, 0x24, (uint32_t[]) { 0xde0 }, 1);
    } else if (mode == 10) {
        h[6] = 4;
    } else if (mode == 11) {
        h[2] = 0x7f800000;
    } else if (mode == 12) {
        uint32_t raw[] = { 8, 8, 0, 1, 0, 0, 0, fbits(100), fbits(100) };

        kona_command(s, 0x6, raw, ARRAY_SIZE(raw));
    } else if (mode == 13) {
        h[2] = h[3] = 0;
        h[4] = fbits(3);
        h[5] = fbits(4);
        bx = by = 64;
        kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
        kona_command(s, 0x1a, (uint32_t[]) { 0x100 }, 1);
    } else if (mode == 14) {
        h[2] = h[3] = fbits(64.5);
        h[4] = fbits(67.5);
        h[5] = fbits(68.5);
        bx = by = -1;
    }
    if (mode == 18 || mode == 19) {
        h[7] = 3137;
        h[6] = DIV_ROUND_UP(h[7], 4) + 1;
    } else if (mode == 20) {
        h[1] = 6;
        h[7] = 6;
    }
    if (mode == 5 || mode == 6 || mode == 15) {
        kona_command(s, 0x6, move, ARRAY_SIZE(move));
        if (mode == 15) {
            kona_command(s, 0x25, (uint32_t[]) { 0xde0 }, 1);
        }
        bx += 5;
        by += 6;
        px += 5;
        py += 6;
    }
    kona_command(s, 0x5, h, ARRAY_SIZE(h));
    qtest_writel(s, REG_COMMAND, 0x81422418);
    if (migration) {
        tmpfs = g_dir_make_tmp("sn0-kona-bitmap-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
        to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (mode == 16) {
        qtest_writel(s, REG_COMMAND + 0xc + 0x20 * 0x1fc, 0);
        return_context_restore(s, SAVE_AREA);
    } else {
        /* This is payload, even though its word is a valid Begin selector. */
        qtest_writel(s, REG_COMMAND, mode == 20 ? 0x80010057 : 0x00000057);
        for (unsigned k = 2; k < DIV_ROUND_UP(h[7], 4); k++) {
            qtest_writel(s, REG_COMMAND, 0x00000057);
        }
    }
    if (mode == 7) {
        gl_end(s);
    } else if (mode == 9) {
        kona_command(s, 0x25, (uint32_t[]) { 0xde0 }, 1);
    }
    if (mode == 17) {
        return_context_deactivate(s);
        attrib_context_fresh(s, SAVE_AREA + 0x20000);
        gl_window_cleared(s, 0, 0, 0);
        kona_command_f(s, 0xa9, (float[]) { 0, 0 }, 2);
        kona_command(s, 0x6, move, ARRAY_SIZE(move));
        return_context_deactivate(s);
        return_context_restore(s, SAVE_AREA);
        /* Clearing the new context's coincident window erased the bitmap. */
        refused = true;
    }
    if (refused && mode != 17) {
        px = py = 64;
    }
    kona_command_f(s, 0x2f, (float[]) { 1, 1 }, 2);
    kona_command(s, 0x3a, (uint32_t[]) { 2, 2, 5, 1, 0, 0, 0, 0 }, 8);
    for (unsigned k = 0; k < 4; k++) {
        static const uint32_t rgba[] = {
            0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
        };

        qtest_writel(s, REG_COMMAND, rgba[k]);
    }
    bitmap_expected(s, !invalid && !refused, !invalid && mode != 8,
                    bx, by, px, py, mode == 20);
    if (mode == 13) {
        g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, 0xbfffff);
    }
    if (invalid) {
        kona_command(s, 0xc9, (uint32_t[]) { 1 }, 1);
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 4 + 13 * 4), ==, fbits(64));
        g_assert_cmphex(guest_word(s, RETURN_PAGE + 4 + 14 * 4), ==, fbits(64));
    }
    if (to) {
        qtest_quit(to);
    }
    qtest_quit(from);
    if (tmpfs) {
        g_rmdir(tmpfs);
    }
}

static void test_bitmap_stale_destination(void)
{
    g_autofree char *tmpfs = NULL, *uri = NULL, *second_uri = NULL;
    GError *err = NULL;
    QTestState *from, *to, *again;
    uint32_t h[] = { 8, 8, 0, 0, 0, 0, 3, 8 };

    tmpfs = g_dir_make_tmp("sn0-kona-bitmap-stale-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/first.sock", tmpfs);
    second_uri = g_strdup_printf("unix:%s/second.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_initf("%s -incoming %s", MACHINE_ARGS, uri);
    gl_make_current(from);
    gl_window_cleared(from, 0, 0, 0);
    kona_command_f(from, 0xa9, (float[]) { 0, 0 }, 2);
    gl_make_current(to);
    gl_window_cleared(to, 0, 0, 0);
    kona_command_f(to, 0xa9, (float[]) { 0, 0 }, 2);
    kona_command(to, 0x5, h, ARRAY_SIZE(h));
    qtest_writel(to, REG_COMMAND, 0xffffffff);
    sgi_migrate_stopped(from, uri);
    /* An absent bitmap subsection must not retain the destination's prefix. */
    again = qtest_initf("%s -incoming %s", MACHINE_ARGS, second_uri);
    sgi_migrate_stopped(to, second_uri);
    kona_command(again, 0x3a,
                 (uint32_t[]) { 2, 2, 5, 1, 0, 0, 0, 0 }, 8);
    qtest_writel(again, REG_COMMAND, 0xff0000ff);
    qtest_writel(again, REG_COMMAND, 0x00ff00ff);
    qtest_writel(again, REG_COMMAND, 0x0000ffff);
    qtest_writel(again, REG_COMMAND, 0xffffffff);
    draw_pixels_expected(again, true);
    qtest_quit(again);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_texture_native_dma(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool contiguous = mode == 0 || mode == 10 || mode == 6 ||
                      mode == 11 || mode == 13;
    bool skips = mode >= 13;
    bool fault = mode == 2 || mode == 19;
    bool large = mode == 6 || mode == 11;
    bool migration = mode == 7 || mode == 8 || mode == 16;
    bool reset = mode == 9;
    bool trace = mode == 12;
    uint32_t width = mode == 11 ? 4096 : large ? 2048 : 2;
    uint32_t row_bytes = width * 3;
    uint32_t stride = mode == 10 ? 3 : mode == 13 ? 6 :
                      mode == 19 ? 16384 : large ? row_bytes :
                      contiguous ? 8 : 9;
    uint32_t span = stride + row_bytes;
    uint64_t host = UINT64_C(0x380000);
    uint64_t io = mode == 2 ? 0x3ffd : mode == 13 ? 1 :
                  mode == 14 || mode == 19 ? 2 : mode == 15 ? 0 : 3;
    g_autofree uint8_t *source = g_malloc0(span);
    g_autofree char *dir = NULL, *uri = NULL;
    GError *err = NULL;
    g_autofree char *log = NULL;
    QTestState *from = trace ? kona_init_logged(&log) :
                              qtest_init(MACHINE_ARGS);
    QTestState *to = NULL, *s = from;
    uint32_t h[20];
    uint32_t record[22] = { 0x44, 0x101010, 256u << 10 };
    uint32_t total = contiguous ? span : row_bytes * 2;
    uint32_t per_line = contiguous ? span : row_bytes;
    bool refused = fault || (mode >= 3 && mode <= 5) || reset ||
                   mode == 10 || mode == 18;

    gl_make_current(s);
    memcpy(h, texture_header, sizeof(h));
    h[1] = width;
    h[3] = 4;
    h[5] = skips ? 3 : io & 3;
    h[6] = 1;
    h[7] = !contiguous;
    h[9] = 0xd;
    h[13] = width - 1;
    h[16] = mode == 11 ? 0x2c : large ? 0x2b : 0x23;
    h[17] = 0x2204;
    record[3] = h[16];
    kona_command(s, 0x1d5,
                 (uint32_t[]) { 0, 2u << 28, 0xd << 14,
                                mode == 11 ? 0x1c : large ? 0x1b : 0x11,
                                1 }, 5);
    kona_command(s, 0x179, record, 22);
    kona_command(s, 0x33,
                 (uint32_t[]) { 0xcf2, mode == 10 ? 1 :
                                large ? width : contiguous ? 2 : 3 }, 2);
    kona_command(s, 0x33,
                 (uint32_t[]) { 0xcf5,
                                mode == 10 || skips ? (mode == 19 ? 4 : 1) :
                                contiguous ? 4 : 1 }, 2);
    if (skips) {
        kona_command(s, 0x33,
                     (uint32_t[]) { 0xcf3, mode == 13 || mode == 15 ||
                                            mode == 16 }, 2);
        kona_command(s, 0x33,
                     (uint32_t[]) { 0xcf4, mode == 14 || mode == 16 ||
                                            mode == 17 || mode == 18 ||
                                            mode == 19 }, 2);
        if (mode == 17 || mode == 18) {
            io = 2;
        }
        if (mode == 19) {
            kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 5461 }, 2);
        }
        if (mode == 17) {
            pixel_transfer_set(s, 0xd14, 0.5);
        }
    }
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
    if (skips) {
        kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 1 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 0 }, 2);
        kona_command(s, 0x33, (uint32_t[]) { 0xcf4, 0 }, 2);
        if (mode == 17) {
            pixel_transfer_set(s, 0xd14, 0);
        }
    }
    if (mode == 3) {
        total = per_line;
    } else if (mode == 4) {
        total += per_line;
    } else if (mode == 5 || mode == 18) {
        io++;
    }
    if (reset) {
        qtest_system_reset(s);
    }
    if (mode != 7 && mode != 16) {
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
        if (mode == 7 || mode == 16) {
            texture_dma_packet(s, io, total, per_line, stride - row_bytes);
            g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
        }
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    assert_texture_pixel(s, 0, 0,
                          refused ? 0 : mode == 17 ? 0x200000ff : 0x400000ff);
    assert_texture_pixel(s, width - 1, 1,
                          refused ? 0 : (large ? 0x7f0000ff :
                                        mode == 17 ? 0x220000ff : 0x430000ff));
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

/*
 * Where test_texture_migrates() cuts its upload, counted in words of the
 * 20-word header and 8-word payload: nothing, mid-header, one header word
 * short, header complete, first payload word, mid-payload and one word
 * short of the end.
 */
static const unsigned texture_splits[] = { 0, 10, 19, 20, 21, 24, 27 };

static void test_texture_migrates(void)
{
    uint32_t h[20];

    memcpy(h, texture_header, sizeof(h));
    h[2] = 4;
    for (unsigned i = 0; i < ARRAY_SIZE(texture_splits); i++) {
        unsigned split = texture_splits[i];
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
            if (fields[k] == 0) {
                h[0] = 0xde2; /* Neither a 1D, 2D nor 3D target. */
            } else if (fields[k] == 4) {
                h[4] = 7; /* All native component types 0 through 6 work. */
            } else if (fields[k] == 6) {
                h[15] = 1; /* DMA with an unsupported 2D destination. */
            } else if (fields[k] == 7) {
                h[7] = 2; /* Both zero and one have known source framing. */
            }
        } else {
            return_context_deactivate(s);
        }
        prepare_refused_reply(s);
        kona_command(s, 0x174, h, 20);
        if (k < ARRAY_SIZE(fields) && h[0] == 0xde1 && h[4] == 1 && !h[6] &&
            !h[7] && !h[8]) {
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
    g_assert_false(log_shows_misframing(found));
    g_assert_cmpuint(count_occurrences(found, "native texture header refused:"),
                     ==, ARRAY_SIZE(fields) + 1);
    g_assert_cmpuint(count_occurrences(found, "payload length unknown"), ==, 5);
    g_assert_cmpuint(count_occurrences(found, "draining framed rows"),
                     ==, ARRAY_SIZE(fields) - 5);
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

/*
 * The digests of planes 0 to 6, sixty-four hex digits a plane, in one
 * string: the bulk form of renderer_plane_digest() over those planes.
 */
static char *texture_plane_digest_string(QTestState *s)
{
    QDict *reply;
    char *all;

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane','value':0}}");
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane-count','value':7}}");
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane-digests'}}");
    all = g_strdup(qdict_get_str(reply, "return"));
    qobject_unref(reply);
    g_assert_cmpuint(strlen(all), ==, 7 * 64);
    return all;
}

static void texture_plane_digests(QTestState *s, char *digests[7])
{
    g_autofree char *all = texture_plane_digest_string(s);

    for (unsigned plane = 0; plane < 7; plane++) {
        g_assert_cmpint(all[plane * 64], !=, '-');
        digests[plane] = g_strndup(all + plane * 64, 64);
    }
}

static void texture_assert_planes(QTestState *s, char *digests[7])
{
    g_autofree char *all = texture_plane_digest_string(s);

    for (unsigned plane = 0; plane < 7; plane++) {
        g_autofree char *after = g_strndup(all + plane * 64, 64);

        g_assert_cmpint(after[0], !=, '-');
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
    char *words;
    unsigned configs, uploads;

    texture_canaries(s, before);
    for (unsigned image = 0; image < 2; image++) {
        prepare_refused_reply(s);
        qtest_writel(s, REG_COMMAND, 0x1c4);
        qtest_writel(s, REG_COMMAND, image);
        kona_command(s, 0x174, texture_8x8_headers[image], 20);
        texture_words(s, texture_8x8_sources[image], 64, 1);
        assert_texture_staging(s, NULL, 0, 0);
        words = staged_words(s);
        for (unsigned k = 0; k < 64; k++) {
            staged[image][k] = staged_word_at(words, k);
        }
        g_free(words);
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

/*
 * Where the reset and migration cases cut an 8x8 upload short, counted in
 * words delivered out of the 20-word header and 64-word payload: nothing,
 * mid-header, one header word short, header complete, first payload word,
 * mid-payload and one word short of the end. These are the points where
 * the partial-command state changes shape; the positions between them
 * repeat a neighbour's case at the cost of a machine (or a machine pair)
 * each.
 */
static const unsigned texture_8x8_splits[] = { 0, 10, 19, 20, 21, 52, 83 };

static void test_texture_8x8_reset(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t hostile[64];

    for (unsigned k = 0; k < 64; k++) {
        hostile[k] = texture_hostile[k % ARRAY_SIZE(texture_hostile)];
    }
    for (unsigned image = 0; image < 2; image++) {
        for (unsigned i = 0; i < ARRAY_SIZE(texture_8x8_splits); i++) {
            unsigned split = texture_8x8_splits[i];
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
        for (unsigned i = 0; i < ARRAY_SIZE(texture_8x8_splits); i++) {
            unsigned split = texture_8x8_splits[i];
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

static void test_texture_8x8_refused(gconstpointer opaque)
{
    bool known = GPOINTER_TO_UINT(opaque);
    static const unsigned known_fields[] = { 9, 13, 15, 17 };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint32_t h[20], record[22] = { 0x43, 0x101010, 256u << 10, 0x33 };
    uint32_t hostile[64];
    char *before[7];
    unsigned cases = 12, drains = 0, unknowns = 24;

    if (known) {
        cases = ARRAY_SIZE(known_fields);
        drains = 2 * cases;
        unknowns = 0;
    }

    texture_canaries(s, before);
    for (unsigned k = 0; k < ARRAY_SIZE(hostile); k++) {
        hostile[k] = texture_hostile[k % ARRAY_SIZE(texture_hostile)];
    }
    for (unsigned image = 0; image < 2; image++) {
        kona_command(s, 0x174, texture_8x8_headers[image], 20);
        texture_words(s, texture_8x8_sources[image], 64, 1);
        kona_command(s, 0x176, &image, 1);
        kona_command(s, 0x1d5,
                     (uint32_t[]) { image, (2u << 28) | (41 << 12),
                                    0x30 << 14, 0x33, 1 }, 5);
        kona_command(s, 0x179, record, ARRAY_SIZE(record));
        for (unsigned k = 0; k < 64; k++) {
            assert_texture_pixel(s, k % 8, k / 8,
                                 texture_8x8_sources[image][k]);
        }
        for (unsigned field = 0; field < cases; field++) {
            memcpy(h, texture_8x8_headers[image], sizeof(h));
            if (known) {
                h[known_fields[field]] ^= 1;
                if (known_fields[field] == 17) {
                    h[17] = 0;
                }
            } else {
                switch (field) {
                case 0:
                    h[3] = UINT32_MAX;
                    break;
                case 1:
                    h[4] = UINT32_MAX;
                    break;
                case 2:
                    h[5] = 4;
                    break;
                case 3:
                    h[6] = 2;
                    break;
                case 4:
                    h[1] = 0;
                    break;
                case 5:
                    return_context_deactivate(s);
                    break;
                case 6:
                    h[7] = 2;
                    break;
                case 7:
                    h[0] = UINT32_MAX;
                    break;
                case 8:
                    h[8] = 1;
                    break;
                case 9:
                    h[1] = 8193;
                    break;
                case 10:
                    h[2] = 8193;
                    break;
                case 11:
                    h[2] = 1;
                    kona_command(s, 0x33,
                                 (uint32_t[]) { 0xcf2, UINT32_MAX }, 2);
                    break;
                }
            }
            g_test_message("image %u, %s source-span case %u", image,
                           known ? "known" : "unknown", field);
            prepare_refused_reply(s);
            kona_command(s, 0x174, h, ARRAY_SIZE(h));
            if (known) {
                /* Eight RGBA/UBYTE rows of eight pixels, phase zero. */
                texture_words(s, hostile, 63, 1);
                assert_texture_staging(s, NULL, 0, 63);
                assert_reply_unchanged(s);
                texture_assert_planes(s, before);
                qtest_writeq(s, REG_COMMAND,
                             ((uint64_t)hostile[63] << 32) | 0x176);
                qtest_writel(s, REG_COMMAND, image);
            } else {
                /* No payload count is established for these headers. */
                if (field == 5) {
                    return_context_restore(s, SAVE_AREA);
                } else if (field == 11) {
                    kona_command(s, 0x33, (uint32_t[]) { 0xcf2, 0 }, 2);
                }
                kona_command(s, 0x176, &image, 1);
            }
            assert_texture_staging(s, NULL, 0, 0);
            assert_reply_unchanged(s);
            texture_assert_planes(s, before);
            for (unsigned k = 0; k < 64; k++) {
                assert_texture_pixel(s, k % 8, k / 8,
                                     texture_8x8_sources[image][k]);
            }
            assert_native_query_word(s, 0xb92, 0x207, false);
        }
    }
    texture_free_digests(before);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_cmpuint(count_occurrences(found, "native texture header refused:"),
                     ==, 2 * cases);
    g_assert_cmpuint(count_occurrences(found, "draining framed rows"),
                     ==, drains);
    g_assert_cmpuint(count_occurrences(found, "payload length unknown"),
                     ==, unknowns);
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

static uint64_t renderer_fallback_reason(QTestState *s, const char *reason)
{
    g_autofree char *name = g_strdup_printf("x-gpu-fallbacks-%s", reason);

    return renderer_counter(s, name);
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

static void renderer_typed_setup(QTestState *s)
{
    gl_make_current(s);
    kona_command_kicked(s, 0x1f3,
                        (uint32_t[]){ 0, 0, 0x05670080, 0x05670080,
                                      0, 0, FB_WIDTH, FB_HEIGHT, 0, 0xd, 0 },
                        11);
    kona_command_kicked(s, 0x1ee, (uint32_t[]){ 0, 0, 0 }, 3);
    kona_command(s, 0xe3, (uint32_t[]){ 0, 0, FB_WIDTH, FB_HEIGHT }, 4);
    kona_command(s, 0x25, (uint32_t[]){ 0xbd0 }, 1);
    kona_command(s, 0xc0, (uint32_t[]){ 0x1d01 }, 1);
}

static uint32_t renderer_plane_at(QTestState *s, unsigned plane,
                                  unsigned x, unsigned y)
{
    QDict *reply;
    uint32_t value;

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane','value':%u}}", plane);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-x','value':%u}}", x);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-pixel-y','value':%u}}", y);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-plane-word'}}");
    value = qdict_get_int(reply, "return");
    qobject_unref(reply);
    return value;
}

/* An indexed pixel with the same RGB still needs a direct-colour tag. */
static void test_renderer_typed_coverage(gconstpointer opaque)
{
    bool same_rgb = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    uint64_t before, generation;

    renderer_typed_setup(s);
    x_read_fixture_command(s, 0x129,
                           (uint32_t[]){ 0, 0, 0, 0, 0xf00fff, 0,
                                         0x180c, 0x40001, 1, 6, 5, 1, 1 },
                           13);
    x_read_fixture_command(s, 0x136,
                           (uint32_t[]){ 0, 0, 1, 8, 1, 1, 0 }, 7);
    x_read_fixture_command(s, 0x139,
                           (uint32_t[]){ 640, FB_HEIGHT - 1 - 512 }, 2);
    qtest_writel(s, KONA_REG(0xc1000c), 0);
    x_read_fixture_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    x_read_fixture_command(s, 0x139,
                           (uint32_t[]){ 900, FB_HEIGHT - 1 - 700 }, 2);
    qtest_writel(s, KONA_REG(0xc1000c), 0);
    x_read_fixture_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(renderer_plane_at(s, 2, 640, 512), ==, 3);
    g_assert_cmphex(renderer_plane_at(s, 2, 900, 700), ==, 3);

    kona_command_f(s, 0x63,
                   same_rgb ? (float[]){ 0, 0, 0 } :
                              (float[]){ 1, 0, 0 }, 3);
    generation = renderer_counter(s, "x-test-mesa-front-generation");
    before = renderer_counter(s, "x-test-gpu-requests");
    gl_begin(s, 4);
    gl_vertex2f(s, -0.5, -0.5);
    gl_vertex2f(s, 0.5, -0.5);
    gl_vertex2f(s, 0, 0.5);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests") - before,
                     ==, 1);
    g_assert_cmpuint(renderer_counter(s, "x-test-mesa-front-generation"),
                     >, generation);
    g_assert_cmphex(renderer_plane_at(s, 0, 640, 512), ==,
                    same_rgb ? 0 : 0xff0000);
    g_assert_cmphex(renderer_plane_at(s, 2, 640, 512), ==, 0);
    g_assert_cmphex(renderer_plane_at(s, 0, 900, 700), ==, 0);
    g_assert_cmphex(renderer_plane_at(s, 2, 900, 700), ==, 3);

    generation = renderer_counter(s, "x-test-mesa-front-generation");
    x_read_fixture_command(s, 0x139,
                           (uint32_t[]){ 640, FB_HEIGHT - 1 - 512 }, 2);
    qtest_writel(s, KONA_REG(0xc1000c), 0);
    x_read_fixture_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(renderer_plane_at(s, 0, 640, 512), ==, 0);
    g_assert_cmphex(renderer_plane_at(s, 2, 640, 512), ==, 3);
    before = renderer_counter(s, "x-test-gpu-requests");
    gl_begin(s, 4);
    gl_vertex2f(s, -0.5, -0.5);
    gl_vertex2f(s, 0.5, -0.5);
    gl_vertex2f(s, 0, 0.5);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests") - before,
                     ==, 1);
    g_assert_cmpuint(renderer_counter(s, "x-test-mesa-front-generation"),
                     >, generation);
    g_assert_cmphex(renderer_plane_at(s, 0, 640, 512), ==,
                    same_rgb ? 0 : 0xff0000);
    g_assert_cmphex(renderer_plane_at(s, 2, 640, 512), ==, 0);
    g_assert_cmphex(renderer_plane_at(s, 2, 900, 700), ==, 3);

    /*
     * An enabled texture with no image keeps the draw off the Mesa route.
     * The legacy renderer draws it untextured, one request per tile.
     */
    generation = renderer_counter(s, "x-test-mesa-front-generation");
    before = renderer_counter(s, "x-test-gpu-requests");
    kona_command(s, 0x24, (uint32_t[]){ 0xde1 }, 1);
    gl_begin(s, 4);
    gl_vertex2f(s, -0.5, -0.5);
    gl_vertex2f(s, 0.5, -0.5);
    gl_vertex2f(s, 0, 0.5);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-mesa-front-generation"), ==,
                     generation);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests") - before,
                     >=, 1);
    qtest_quit(s);
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

static void renderer_failure_quad(QTestState *s)
{
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
}

static void test_renderer_legacy_failure(void)
{
    g_autofree char *log = NULL, *found = NULL;
    g_autofree Expected *expected = g_new(Expected, 1);
    QTestState *s = kona_init_logged_args(&log,
                                         " -global sgi-kona.host-gpu=on");
    char *planes[7];
    uint64_t requests, draws, fallbacks, completions;
    uint32_t causes;
    QDict *status;

    renderer_fixture(s);
    kona_command(s, 0x2e, (uint32_t[]){ 0x1506 }, 1);
    kona_command(s, 0x24, (uint32_t[]){ 0xbf2 }, 1);
    kona_command_f(s, 0x63, (float[]){ 0.8f, 0.8f, 0.8f }, 3);
    texture_plane_digests(s, planes);
    prepare_refused_reply(s);
    requests = renderer_counter(s, "x-test-gpu-requests");
    draws = renderer_counter(s, "x-test-gpu-draws");
    fallbacks = renderer_counter(s, "x-test-gpu-fallbacks");
    completions = renderer_counter(s, "x-test-dma-completions");
    causes = qtest_readl(s, REG_INT_CAUSE) & (INT_CAUSE_MAILBOX | 0x20);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gpu-fail-draw','value':2}}");
    renderer_failure_quad(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     requests + 2);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, draws);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     fallbacks);
    status = qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");
    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
    qobject_unref(status);
    texture_assert_planes(s, planes);
    assert_reply_unchanged(s);

    /* Failed host staging cannot be retried into native pixels or a fence. */
    renderer_failure_quad(s);
    kona_command(s, 0xc6, (uint32_t[]){ 1 }, 1);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     requests + 2);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, draws);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     fallbacks);
    g_assert_cmpuint(renderer_counter(s, "x-test-dma-completions"), ==,
                     completions);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) &
                    (INT_CAUSE_MAILBOX | 0x20), ==, causes);
    texture_assert_planes(s, planes);
    assert_reply_unchanged(s);
    texture_free_digests(planes);

    qtest_system_reset(s);
    renderer_fixture(s);
    kona_command(s, 0x2e, (uint32_t[]){ 0x1506 }, 1);
    kona_command(s, 0x24, (uint32_t[]){ 0xbf2 }, 1);
    kona_command_f(s, 0x63, (float[]){ 0.8f, 0.8f, 0.8f }, 3);
    requests = renderer_counter(s, "x-test-gpu-requests");
    draws = renderer_counter(s, "x-test-gpu-draws");
    renderer_failure_quad(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     requests + 2);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, draws + 1);
    expected_clear(expected, 0x333333);
    assert_window_matches(s, expected);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found,
        "sgi-kona: legacy renderer draw failed: "
        "Injected SGI renderer test draw failure"));
    g_assert_null(strstr(found, "sgi_kona_return_post token 0xc6 "));
}

static void assert_native_error(QTestState *s, uint32_t error);
static void renderer_assert_image(QTestState *s);
static void renderer_assert_image_colour(QTestState *s, uint32_t rgb);

static void renderer_create_triangle(QTestState *s, bool typed)
{
    if (typed) {
        gl_begin(s, 4);
        gl_vertex2f(s, -0.5, -0.5);
        gl_vertex2f(s, 0.5, -0.5);
        gl_vertex2f(s, 0, 0.5);
        gl_end(s);
    } else {
        renderer_triangle(s);
    }
}

/* The one-shot constructor result is host-only and requires no host OOM. */
static void test_renderer_create_failure(gconstpointer opaque)
{
    unsigned variant = GPOINTER_TO_UINT(opaque);
    bool typed = variant > 3;
    unsigned failure = typed ? variant - 4 : variant;
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged_args(&log,
                                         " -global sgi-kona.host-gpu=on");
    char *planes[7];
    uint64_t requests, draws, fallbacks, completions;
    uint32_t causes;
    QDict *status;

    if (typed) {
        renderer_typed_setup(s);
    } else {
        renderer_fixture(s);
        kona_command(s, 0x2e, (uint32_t[]){ 0x1506 }, 1);
        kona_command(s, 0x24, (uint32_t[]){ 0xbf2 }, 1);
    }
    kona_command_f(s, 0x63, (float[]){ 1, 0, 0 }, 3);
    texture_plane_digests(s, planes);
    prepare_refused_reply(s);
    requests = renderer_counter(s, "x-test-gpu-requests");
    draws = renderer_counter(s, "x-test-gpu-draws");
    fallbacks = renderer_counter(s, "x-test-gpu-fallbacks");
    completions = renderer_counter(s, "x-test-dma-completions");
    causes = qtest_readl(s, REG_INT_CAUSE) & (INT_CAUSE_MAILBOX | 0x20);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gpu-fail-create','value':%u}}", failure);
    renderer_create_triangle(s, typed);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     requests + (typed ? 0 : 1));
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, draws);

    if (failure == 1) {
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                         fallbacks + 1);
        g_assert_cmphex(renderer_plane_at(s, 0,
                                          typed ? 640 : PROBE_X + 32,
                                          typed ? 512 : PROBE_Y + 32), ==,
                        typed ? 0xff0000 : 0x00ffff);
        if (!typed) {
            renderer_assert_image_colour(s, 0x00ffff);
        }
    } else {
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                         fallbacks);
        texture_assert_planes(s, planes);
        assert_reply_unchanged(s);
        g_assert_cmpuint(renderer_counter(s, "x-test-dma-completions"), ==,
                         completions);
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) &
                        (INT_CAUSE_MAILBOX | 0x20), ==, causes);
        if (failure == 2) {
            assert_native_error(s, 0x505);
            assert_native_error(s, 0);
            renderer_create_triangle(s, typed);
            g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                             requests + (typed ? 1 : 2));
            g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==,
                             draws + 1);
            g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                             fallbacks);
            if (typed) {
                g_assert_cmphex(renderer_plane_at(s, 0, 640, 512), ==,
                                0xff0000);
                g_assert_cmphex(renderer_plane_at(s, 0, 900, 700), ==, 0);
            } else {
                renderer_assert_image_colour(s, 0x00ffff);
            }
            assert_native_error(s, 0);
        } else {
            status = qtest_qmp_assert_success_ref(s,
                                                  "{'execute':'query-status'}");
            g_assert_cmpstr(qdict_get_str(status, "status"), ==,
                            "internal-error");
            qobject_unref(status);
            renderer_create_triangle(s, typed);
            kona_command(s, 0xc6, (uint32_t[]){ 1 }, 1);
            g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==,
                             draws);
            g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                             fallbacks);
            g_assert_cmpuint(renderer_counter(s, "x-test-dma-completions"), ==,
                             completions);
            texture_assert_planes(s, planes);
            assert_reply_unchanged(s);
        }
    }
    texture_free_digests(planes);
    qtest_system_reset(s);
    if (typed) {
        renderer_typed_setup(s);
    } else {
        renderer_fixture(s);
        kona_command(s, 0x2e, (uint32_t[]){ 0x1506 }, 1);
        kona_command(s, 0x24, (uint32_t[]){ 0xbf2 }, 1);
    }
    kona_command_f(s, 0x63, (float[]){ 1, 0, 0 }, 3);
    draws = renderer_counter(s, "x-test-gpu-draws");
    renderer_create_triangle(s, typed);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, draws + 1);
    if (!typed) {
        renderer_assert_image_colour(s, 0x00ffff);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    if (failure == 3 && !typed) {
        g_assert_nonnull(strstr(found,
            "sgi-kona: legacy renderer draw failed: "
            "Injected SGI renderer creation failure"));
    }
}

static void test_renderer_create_arm_cleared_on_load(void)
{
    g_autofree char *dir = NULL, *uri = NULL;
    char *planes[7];
    GError *err = NULL;
    QTestState *from, *to;
    uint64_t requests, draws, fallbacks;

    dir = g_dir_make_tmp("sn0-kona-create-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    from = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    renderer_fixture(from);
    kona_command(from, 0x2e, (uint32_t[]){ 0x1506 }, 1);
    kona_command(from, 0x24, (uint32_t[]){ 0xbf2 }, 1);
    kona_command_f(from, 0x63, (float[]){ 1, 0, 0 }, 3);
    requests = renderer_counter(from, "x-test-gpu-requests");
    draws = renderer_counter(from, "x-test-gpu-draws");
    fallbacks = renderer_counter(from, "x-test-gpu-fallbacks");
    texture_plane_digests(from, planes);
    qtest_qmp_assert_success(from,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gpu-fail-create','value':2}}");

    to = qtest_initf(MACHINE_ARGS " -incoming %s"
                     " -global sgi-kona.host-gpu=on", uri);
    qtest_qmp_assert_success(to,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gpu-fail-create','value':2}}");
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    texture_assert_planes(to, planes);
    g_assert_cmpuint(renderer_counter(to, "x-test-gpu-requests"), ==, 0);
    g_assert_cmpuint(renderer_counter(to, "x-test-gpu-draws"), ==, 0);
    g_assert_cmpuint(renderer_counter(to, "x-test-gpu-fallbacks"), ==, 0);
    renderer_triangle(to);
    g_assert_cmpuint(renderer_counter(to, "x-test-gpu-requests"), ==, 1);
    g_assert_cmpuint(renderer_counter(to, "x-test-gpu-draws"), ==, 1);
    g_assert_cmpuint(renderer_counter(to, "x-test-gpu-fallbacks"), ==, 0);
    renderer_assert_image_colour(to, 0x00ffff);
    assert_native_error(to, 0);
    g_assert_cmpuint(renderer_counter(from, "x-test-gpu-requests"), ==,
                     requests);
    g_assert_cmpuint(renderer_counter(from, "x-test-gpu-draws"), ==, draws);
    g_assert_cmpuint(renderer_counter(from, "x-test-gpu-fallbacks"), ==,
                     fallbacks);
    texture_assert_planes(from, planes);
    texture_free_digests(planes);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_renderer_typed_ortho_excluded(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    uint64_t generation;

    renderer_typed_setup(s);
    kona_command(s, 0xd0, (uint32_t[]){ 0x1701 }, 1);
    kona_command_d(s, 0xd7,
                   (double[]){ 0, FB_WIDTH, 0, FB_HEIGHT, -1, 1 }, 6);
    kona_command(s, 0xd0, (uint32_t[]){ 0x1700 }, 1);
    generation = renderer_counter(s, "x-test-mesa-front-generation");
    gl_begin(s, 4);
    gl_vertex2f(s, 320, 256);
    gl_vertex2f(s, 960, 256);
    gl_vertex2f(s, 640, 768);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-mesa-front-generation"),
                     ==, generation);
    qtest_quit(s);
}

static void test_renderer_typed_failure_reset(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    QDict *reply;
    uint64_t before, generation;

    renderer_typed_setup(s);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-mesa-failed','value':true}}");
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-mesa-failed'}}");
    g_assert_true(qdict_get_bool(reply, "return"));
    qobject_unref(reply);
    before = renderer_counter(s, "x-test-gpu-requests");
    gl_begin(s, 4);
    gl_vertex2f(s, -0.5, -0.5);
    gl_vertex2f(s, 0.5, -0.5);
    gl_vertex2f(s, 0, 0.5);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, before);
    qtest_system_reset(s);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-mesa-failed'}}");
    g_assert_false(qdict_get_bool(reply, "return"));
    qobject_unref(reply);

    renderer_typed_setup(s);
    generation = renderer_counter(s, "x-test-mesa-front-generation");
    before = renderer_counter(s, "x-test-gpu-requests");
    gl_begin(s, 4);
    gl_vertex2f(s, -0.5, -0.5);
    gl_vertex2f(s, 0.5, -0.5);
    gl_vertex2f(s, 0, 0.5);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests") - before,
                     ==, 1);
    g_assert_cmpuint(renderer_counter(s, "x-test-mesa-front-generation"),
                     >, generation);
    qtest_quit(s);
}

/*
 * A Mesa-route draw into a window smaller than the screen completes on the
 * host and leaves pixels outside the window untouched.
 */
static void test_renderer_typed_partial_window(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    uint64_t generation, requests, fallbacks;

    renderer_typed_setup(s);
    x_read_fixture_command(s, 0x129,
                           (uint32_t[]){ 0, 0, 0, 0, 0xf00fff, 0,
                                         0x180c, 0x40001, 1, 6, 5, 1, 1 },
                           13);
    x_read_fixture_command(s, 0x136,
                           (uint32_t[]){ 0, 0, 1, 8, 1, 1, 0 }, 7);
    for (unsigned x = 600; x <= 640; x += 40) {
        x_read_fixture_command(s, 0x139,
                               (uint32_t[]){ x, FB_HEIGHT - 1 - 512 }, 2);
        qtest_writel(s, KONA_REG(0xc1000c), 0);
        x_read_fixture_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
        g_assert_cmphex(renderer_plane_at(s, 2, x, 512), ==, 3);
    }
    kona_command_kicked(s, 0x1f3,
                        (uint32_t[]){ 0, 0, 0x05670080, 0x05670080,
                                      0, 0, 620, 600, 0, 0xd, 0 }, 11);
    kona_command_kicked(s, 0x1ee, (uint32_t[]){ 0, 0, 0 }, 3);
    kona_command_f(s, 0x63, (float[]){ 1, 0, 0 }, 3);
    generation = renderer_counter(s, "x-test-mesa-front-generation");
    requests = renderer_counter(s, "x-test-gpu-requests");
    fallbacks = renderer_counter(s, "x-test-gpu-fallbacks");
    gl_begin(s, 4);
    gl_vertex2f(s, -0.5, -0.5);
    gl_vertex2f(s, 0.5, -0.5);
    gl_vertex2f(s, 0, 0.5);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     requests + 1);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     fallbacks);
    g_assert_cmpuint(renderer_counter(s, "x-test-mesa-front-generation"),
                     >, generation);
    g_assert_cmphex(renderer_plane_at(s, 0, 600, 512), ==, 0xff0000);
    g_assert_cmphex(renderer_plane_at(s, 2, 600, 512), ==, 0);
    g_assert_cmphex(renderer_plane_at(s, 0, 640, 512), ==, 0);
    g_assert_cmphex(renderer_plane_at(s, 2, 640, 512), ==, 3);
    qtest_quit(s);
}

/*
 * A failed draw on the Mesa route stops the VM. It is not retried on the
 * scalar path, and a later fence does not complete.
 */
static void test_renderer_typed_checked_failure(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s;
    char *planes[7];
    uint64_t requests, draws, fallbacks, completions, generation;
    uint32_t causes;
    QDict *status;

    if (!g_test_subprocess()) {
        /* The VM-stop diagnostic goes to QEMU's stderr. */
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        g_test_trap_assert_stderr(
            "*Injected SGI renderer test draw failure*");
        g_test_trap_assert_stderr_unmatched(
            "*Injected SGI renderer test draw failure*"
            "Injected SGI renderer test draw failure*");
        return;
    }
    s = kona_init_logged_args(&log, " -global sgi-kona.host-gpu=on");
    renderer_typed_setup(s);
    kona_command_f(s, 0x63, (float[]){ 1, 0, 0 }, 3);
    texture_plane_digests(s, planes);
    prepare_refused_reply(s);
    requests = renderer_counter(s, "x-test-gpu-requests");
    draws = renderer_counter(s, "x-test-gpu-draws");
    fallbacks = renderer_counter(s, "x-test-gpu-fallbacks");
    completions = renderer_counter(s, "x-test-dma-completions");
    generation = renderer_counter(s, "x-test-mesa-front-generation");
    causes = qtest_readl(s, REG_INT_CAUSE) & (INT_CAUSE_MAILBOX | 0x20);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gpu-fail-draw','value':1}}");
    gl_begin(s, 4);
    gl_vertex2f(s, -0.5, -0.5);
    gl_vertex2f(s, 0.5, -0.5);
    gl_vertex2f(s, 0, 0.5);
    gl_end(s);
    status = qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");
    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
    qobject_unref(status);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     requests + 1);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, draws);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     fallbacks);
    g_assert_cmpuint(renderer_counter(s, "x-test-mesa-front-generation"),
                     >, generation);
    /* A failure is not a refusal under any reason. */
    g_assert_cmpuint(renderer_fallback_reason(s, "precision"), ==, 0);
    g_assert_cmpuint(renderer_fallback_reason(s, "mixed-frame"), ==, 0);
    g_assert_cmpuint(renderer_fallback_reason(s, "fog"), ==, 0);
    texture_assert_planes(s, planes);
    assert_reply_unchanged(s);

    gl_begin(s, 4);
    gl_vertex2f(s, -0.5, -0.5);
    gl_vertex2f(s, 0.5, -0.5);
    gl_vertex2f(s, 0, 0.5);
    gl_end(s);
    kona_command(s, 0xc6, (uint32_t[]){ 1 }, 1);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     requests + 1);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, draws);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     fallbacks);
    g_assert_cmpuint(renderer_counter(s, "x-test-dma-completions"), ==,
                     completions);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) &
                    (INT_CAUSE_MAILBOX | 0x20), ==, causes);
    texture_assert_planes(s, planes);
    assert_reply_unchanged(s);
    texture_free_digests(planes);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(found);
    g_assert_null(strstr(found, "sgi_kona_return_post token 0xc6 "));
}

static void renderer_assert_image_colour(QTestState *s, uint32_t rgb)
{
    g_autofree Expected *expected = g_new(Expected, 1);

    expected_clear(expected, 0xffffff);
    for (unsigned y = 16; y < 128; y++) {
        for (unsigned x = 16; x < 128; x++) {
            if (x + y <= 126) {
                unsigned offset = ((127 - y) * 128 + x) * 3;
                expected->rgb[offset] = rgb >> 16;
                expected->rgb[offset + 1] = rgb >> 8;
                expected->rgb[offset + 2] = rgb;
            }
        }
    }
    assert_window_matches(s, expected);
}

static void renderer_assert_image(QTestState *s)
{
    renderer_assert_image_colour(s, 0xff0000);
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
    bool render_node = GPOINTER_TO_UINT(opaque);
    bool require_gpu = render_node ||
                       g_getenv("QEMU_TEST_SGI_REQUIRE_MESA") != NULL;
    bool using_gpu;
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    QTestState *s;
    uint32_t depth, stencil;
    g_autofree char *depth_digest = NULL, *stencil_digest = NULL;
    g_autofree char *tag_digest = NULL, *after = NULL;

    if (render_node && !node) {
        qtest_prereq_missing("SGI_RENDER_NODE",
                             "QEMU_TEST_SGI_RENDER_NODE is not set");
        return;
    }
    if (render_node) {
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
    using_gpu = renderer_counter(s, "x-test-gpu-draws") == 1;
    if (require_gpu) {
        g_assert_true(using_gpu);
    }
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==,
                     using_gpu ? 1 : 0);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     using_gpu ? 0 : 1);
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
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==,
                     using_gpu ? 2 : 0);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     using_gpu ? 0 : 2);
    /* An explicitly excluded dither state executes only through the CPU. */
    kona_command(s, 0x24, (uint32_t[]){ 0xbd0 }, 1);
    renderer_triangle(s);
    renderer_assert_image(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==,
                     using_gpu ? 2 : 0);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     using_gpu ? 1 : 3);
    g_assert_cmpuint(renderer_fallback_reason(s, "dither"), ==, using_gpu);
    /* So does one-dimensional texturing, which the host path lacks. */
    qtest_system_reset(s);
    renderer_fixture(s);
    kona_command(s, 0x24, (uint32_t[]){ 0xde0 }, 1);
    renderer_triangle(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==,
                     using_gpu ? 2 : 0);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                     using_gpu ? 2 : 4);
    qtest_system_reset(s);
    renderer_fixture(s);
    renderer_triangle(s);
    renderer_assert_image(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==,
                     using_gpu ? 3 : 0);
    qtest_quit(s);
}

static uint32_t renderer_general_rgb(const uint8_t *frame,
                                     unsigned x, unsigned y)
{
    const uint8_t *p = frame +
        ((FB_HEIGHT - 1 - PROBE_Y - y) * FB_WIDTH + PROBE_X + x) * 3;

    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static void renderer_general_compare_planes(QTestState *gpu, QTestState *cpu,
                                            unsigned mode)
{
    static const unsigned planes[] = { 0, 1, 2, 3, 5, 6 };

    for (unsigned i = 0; i < ARRAY_SIZE(planes); i++) {
        g_autofree char *gpu_digest = renderer_plane_digest(gpu, planes[i]);
        g_autofree char *cpu_digest = renderer_plane_digest(cpu, planes[i]);

        g_test_message("mode %u plane %u", mode, planes[i]);
        g_assert_cmpstr(gpu_digest, ==, cpu_digest);
    }
}

typedef struct RendererGeneralSample {
    unsigned x, y;
    bool inside;
} RendererGeneralSample;

static void renderer_general_compare(QTestState *gpu, QTestState *cpu,
                                     unsigned mode)
{
    static const RendererGeneralSample whole[] = {
        { 127, 1, true }, { 96, 32, true },
        { 32, 96, false }, { 0, 127, false },
    };
    static const RendererGeneralSample translated[] = {
        { 95, 65, true }, { 90, 70, true },
        { 70, 90, false }, { 64, 95, false },
    };
    static const RendererGeneralSample perspective[] = {
        { 95, 33, true }, { 80, 48, true },
        { 48, 80, false }, { 32, 95, false },
    };
    const RendererGeneralSample *samples = mode == 6 ? translated :
                                           mode == 8 ? perspective : whole;
    bool covered = mode != 3 && mode != 5 && mode != 9;
    g_autofree uint8_t *gpu_frame = kona_screendump(gpu);
    g_autofree uint8_t *cpu_frame = kona_screendump(cpu);

    renderer_general_compare_planes(gpu, cpu, mode);
    for (unsigned i = 0; i < ARRAY_SIZE(whole); i++) {
        unsigned x = samples[i].x, y = samples[i].y;
        /* gl_window_cleared selects GL_FLAT; vertex 2 provides the colour. */
        uint32_t want = covered && samples[i].inside ? 0x113fbf : 0xffffff;
        uint32_t tag = covered && samples[i].inside ? 0 : 1;

        g_test_message("mode %u sample %u,%u", mode, x, y);
        g_assert_cmphex(renderer_general_rgb(cpu_frame, x, y), ==, want);
        g_assert_cmphex(renderer_general_rgb(gpu_frame, x, y), ==, want);
        g_assert_cmphex(renderer_plane_at(cpu, 2, PROBE_X + x, PROBE_Y + y),
                        ==, tag);
        g_assert_cmphex(renderer_plane_at(gpu, 2, PROBE_X + x, PROBE_Y + y),
                        ==, tag);
    }
}

enum { RENDERER_GENERAL_NO_GPU = 10 };

static void test_renderer_general_triangle(gconstpointer opaque)
{
    unsigned test_case = GPOINTER_TO_UINT(opaque);
    bool no_gpu = test_case == RENDERER_GENERAL_NO_GPU;
    unsigned mode = no_gpu ? 0 : test_case;
    bool host_draw = mode != 5 && mode != 9;
    QTestState *gpu = qtest_initf(MACHINE_ARGS
                                  " -global sgi-kona.host-gpu=%s",
                                  no_gpu ? "off" : "on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { gpu, cpu };
    float vertices[3][2] = { { 0, 0 }, { 128, 0 }, { 128, 128 } };

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_fixture(states[i]);
    }
    renderer_general_compare_planes(gpu, cpu, mode);
    g_assert_cmpuint(renderer_counter(cpu, "x-test-gpu-requests"), ==, 0);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-requests"), ==, 0);
    if (mode == 1) {
        vertices[0][0] = vertices[0][1] = .001;
    } else if (mode == 2) {
        float vertex[2];

        memcpy(vertex, vertices[1], sizeof(vertex));
        memcpy(vertices[1], vertices[2], sizeof(vertex));
        memcpy(vertices[2], vertex, sizeof(vertex));
    } else if (mode == 3) {
        vertices[1][0] = vertices[1][1] = 64;
    } else if (mode == 4) {
        vertices[0][0] = -.001;
    } else if (mode == 5) {
        vertices[2][0] = NAN;
    } else if (mode == 6) {
        vertices[1][0] = vertices[2][0] = vertices[2][1] = 32;
    } else if (mode >= 7) {
        vertices[0][0] = vertices[0][1] = -1;
        vertices[1][0] = vertices[2][0] = vertices[2][1] = 1;
        vertices[1][1] = -1;
    }
    for (unsigned state = 0; state < ARRAY_SIZE(states); state++) {
        QTestState *s = states[state];

        if (mode == 6) {
            kona_command_f(s, 0xdf, (float[]) { 64, 64, 0 }, 3);
        } else if (mode >= 7) {
            kona_command(s, 0xd0, (uint32_t[]) { 0x1701 }, 1);
            kona_command(s, 0xd2, (uint32_t[]) { 0 }, 1);
            if (mode == 8) {
                kona_command_d(s, 0xd1,
                               (double[]) { -1, 1, -1, 1, 1, 10 }, 6);
            }
        }
        gl_begin(s, 4);
        for (unsigned i = 0; i < 3; i++) {
            kona_command_f(s, 0x63, (float[]) {
                i == 2 ? 17 / 255.0f : 1,
                i == 2 ? 63 / 255.0f : 0,
                i == 2 ? 191 / 255.0f : 0,
            }, 3);
            if (mode == 8 || mode == 9) {
                kona_command_f(s, 0x90, (float[]) {
                    vertices[i][0], vertices[i][1], mode == 8 ? -2 : 2,
                }, 3);
            } else {
                gl_vertex2f(s, vertices[i][0], vertices[i][1]);
            }
        }
        gl_end(s);
    }
    renderer_general_compare(gpu, cpu, mode);
    g_assert_cmpuint(renderer_counter(cpu, "x-test-gpu-requests"), ==, 0);
    g_assert_cmpuint(renderer_counter(cpu, "x-test-gpu-draws"), ==, 0);
    g_assert_cmpuint(renderer_counter(cpu, "x-test-gpu-fallbacks"), ==, 0);
    if (no_gpu) {
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-requests"), ==, 0);
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==, 0);
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==, 0);
    } else {
        uint64_t draws = renderer_counter(gpu, "x-test-gpu-draws");
        bool using_gpu = draws == 1;

        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-requests"), ==,
                         mode == 4 ? 2 : host_draw ? 1 : 0);
        g_assert_cmpuint(draws, <=, host_draw ? 1 : 0);
        if (host_draw && g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
            g_assert_true(using_gpu);
        }
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==,
                         using_gpu ? 0 : 1);
    }
    g_test_message("mode %u host-gpu=%s counters: %" PRIu64 "/%" PRIu64
                   "/%" PRIu64, mode, no_gpu ? "off" : "on",
                   renderer_counter(gpu, "x-test-gpu-requests"),
                   renderer_counter(gpu, "x-test-gpu-draws"),
                   renderer_counter(gpu, "x-test-gpu-fallbacks"));
    qtest_quit(cpu);
    qtest_quit(gpu);
}

/*
 * A Mesa-route draw the shared renderer refuses is drawn by the scalar path
 * inside the same callback, exactly as a host-gpu=off board draws it, and
 * is counted under its reason. It does not go on to the legacy host batch:
 * mode 1's alpha test is state that batch would otherwise draw. Mode 2
 * blends under a scissor test, which the shared renderer draws but the
 * scalar comparisons do not prove, so only the mixed-frame option refuses
 * it. Once the refused state is gone the next draw is accepted by Mesa
 * again.
 */
static void test_renderer_mesa_refused(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const uint32_t enables[] = { 0xb60, 0xbc0, 0xbe2 };
    static const char *reasons[] = { "fog", "fragment-tests", "mixed-frame" };
    QTestState *gpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { gpu, cpu };
    uint64_t requests, draws, fallbacks, reason, generation;

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_typed_setup(states[i]);
        kona_command_f(states[i], 0x63, (float[]){ 1, 0.5f, 0 }, 3);
        if (mode == 2) {
            kona_command(states[i], 0xcb, (uint32_t[]){ 0x302, 0x303 }, 2);
            kona_command(states[i], 0xc,
                         (uint32_t[]){ 0, 0, FB_WIDTH, FB_HEIGHT }, 4);
            kona_command(states[i], 0x24, (uint32_t[]){ 0xc11 }, 1);
        }
    }
    requests = renderer_counter(gpu, "x-test-gpu-requests");
    draws = renderer_counter(gpu, "x-test-gpu-draws");
    fallbacks = renderer_counter(gpu, "x-test-gpu-fallbacks");
    reason = renderer_fallback_reason(gpu, reasons[mode]);
    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        kona_command(states[i], 0x24, &enables[mode], 1);
        gl_begin(states[i], 4);
        gl_vertex2f(states[i], -0.5, -0.5);
        gl_vertex2f(states[i], 0.5, -0.5);
        gl_vertex2f(states[i], 0, 0.5);
        gl_end(states[i]);
    }
    renderer_general_compare_planes(gpu, cpu, mode);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-requests"), ==,
                     requests);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==, draws);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==,
                     fallbacks + 1);
    g_assert_cmpuint(renderer_fallback_reason(gpu, reasons[mode]), ==,
                     reason + 1);
    g_assert_cmpuint(renderer_fallback_reason(gpu, "precision"), ==, 0);
    g_assert_cmpuint(renderer_counter(cpu, "x-test-gpu-fallbacks"), ==, 0);

    kona_command(gpu, 0x25, &enables[mode], 1);
    generation = renderer_counter(gpu, "x-test-mesa-front-generation");
    gl_begin(gpu, 4);
    gl_vertex2f(gpu, -0.25, -0.25);
    gl_vertex2f(gpu, 0.25, -0.25);
    gl_vertex2f(gpu, 0, 0.25);
    gl_end(gpu);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-requests"), ==,
                     requests + 1);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==,
                     draws + 1);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==,
                     fallbacks + 1);
    g_assert_cmpuint(renderer_fallback_reason(gpu, reasons[mode]), ==,
                     reason + 1);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-mesa-front-generation"),
                     >, generation);
    qtest_quit(cpu);
    qtest_quit(gpu);
}

/*
 * An enabled texture with no image draws untextured. When the legacy host
 * route takes such a draw, or refuses it after texture preflight (mode 0's
 * fog), the shared incompleteness diagnostic appears once for the draw:
 * the host attempt stays quiet and the path that finally draws reports.
 */
static void test_renderer_texture_incomplete(gconstpointer opaque)
{
    bool refused = !GPOINTER_TO_UINT(opaque);
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged_args(&log,
                                         " -global sgi-kona.host-gpu=on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { s, cpu };
    uint64_t draws, fallbacks;

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_fixture(states[i]);
        kona_command(states[i], 0x24, (uint32_t[]){ 0xde1 }, 1);
        if (refused) {
            kona_command(states[i], 0x24, (uint32_t[]){ 0xb60 }, 1);
        }
    }
    draws = renderer_counter(s, "x-test-gpu-draws");
    fallbacks = renderer_counter(s, "x-test-gpu-fallbacks");
    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_triangle(states[i]);
    }
    renderer_general_compare_planes(s, cpu, refused);
    if (refused) {
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==, draws);
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                         fallbacks + 1);
        g_assert_cmpuint(renderer_fallback_reason(s, "fog"), ==, 1);
    } else {
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws") +
                         renderer_counter(s, "x-test-gpu-fallbacks"), ==,
                         draws + fallbacks + 1);
        if (g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
            g_assert_cmpuint(renderer_counter(s, "x-test-gpu-draws"), ==,
                             draws + 1);
        }
    }
    qtest_quit(cpu);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "sgi-gl: incomplete texture"),
                     ==, 1);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_gpu_fallback "
                                              "reason fog"),
                     ==, refused);
}

/*
 * The legacy route declines a triangle while a context keeps line stipple
 * (mode 0) or line smoothing (mode 1) enabled, draws it on the scalar path
 * exactly as a host-gpu=off board does, and counts it under the shared
 * row: stipple is the primitive row, smoothing its own.
 */
static void test_renderer_legacy_raster_enable(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const uint32_t enables[] = { 0xb24, 0xb20 };
    static const char *reasons[] = { "primitive", "smoothing" };
    QTestState *gpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { gpu, cpu };
    uint64_t draws, fallbacks;

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_fixture(states[i]);
        kona_command(states[i], 0x24, (uint32_t[]){ 0xde1 }, 1);
        kona_command(states[i], 0x24, &enables[mode], 1);
    }
    draws = renderer_counter(gpu, "x-test-gpu-draws");
    fallbacks = renderer_counter(gpu, "x-test-gpu-fallbacks");
    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_triangle(states[i]);
    }
    renderer_general_compare_planes(gpu, cpu, mode);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==, draws);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==,
                     fallbacks + 1);
    g_assert_cmpuint(renderer_fallback_reason(gpu, reasons[mode]), ==, 1);
    g_assert_cmpuint(renderer_fallback_reason(gpu, reasons[!mode]), ==, 0);
    qtest_quit(cpu);
    qtest_quit(gpu);
}

/*
 * A legacy host draw is committed only when the scalar path writes the
 * same values to the same places. With one host pixel skewed the board
 * draws the scalar result instead, exactly as a host-gpu=off board does,
 * and counts a fallback; the next draw is confirmed on the host again.
 */
static void test_renderer_legacy_mismatch(void)
{
    QTestState *gpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { gpu, cpu };
    uint64_t requests, draws, fallbacks;
    bool host;

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_fixture(states[i]);
    }
    requests = renderer_counter(gpu, "x-test-gpu-requests");
    draws = renderer_counter(gpu, "x-test-gpu-draws");
    fallbacks = renderer_counter(gpu, "x-test-gpu-fallbacks");
    qtest_qmp_assert_success(gpu,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gpu-skew-legacy','value':true}}");
    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_triangle(states[i]);
    }
    renderer_general_compare_planes(gpu, cpu, 0);
    host = renderer_counter(gpu, "x-test-gpu-requests") > requests;
    if (g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_assert_true(host);
    }
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==, draws);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==,
                     fallbacks + 1);
    g_assert_cmpuint(renderer_fallback_reason(gpu,
                         host ? "precision" : "unavailable"), ==, 1);

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        kona_command_f(states[i], 0x63, (float[]){ 0, 0, 1 }, 3);
        renderer_triangle(states[i]);
    }
    renderer_general_compare_planes(gpu, cpu, 1);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==,
                     draws + host);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==,
                     fallbacks + 2 - host);
    qtest_quit(cpu);
    qtest_quit(gpu);
}

/*
 * A legacy host draw whose held writes cannot be allocated, for the host
 * batch (nth 1) or the confirming scalar draw (nth 2), is guest GL
 * out-of-memory: nothing reaches the planes and no fallback is counted.
 * The next draw is held and confirmed as usual.
 */
static void test_renderer_hold_oom(gconstpointer opaque)
{
    unsigned nth = GPOINTER_TO_UINT(opaque);
    QTestState *gpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { gpu, cpu };
    uint64_t requests, draws, fallbacks;
    bool host;

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        renderer_fixture(states[i]);
    }
    requests = renderer_counter(gpu, "x-test-gpu-requests");
    draws = renderer_counter(gpu, "x-test-gpu-draws");
    fallbacks = renderer_counter(gpu, "x-test-gpu-fallbacks");
    qtest_qmp_assert_success(gpu,
        "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
        "'property':'x-test-gpu-fail-hold','value':%u}}", nth);
    renderer_triangle(gpu);
    host = renderer_counter(gpu, "x-test-gpu-requests") > requests;
    if (g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_assert_true(host);
    }
    if (host) {
        renderer_general_compare_planes(gpu, cpu, nth);
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==,
                         draws);
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==,
                         fallbacks);
        assert_native_error(gpu, 0x505);
        assert_native_error(gpu, 0);
        renderer_triangle(gpu);
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==,
                         draws + 1);
    }
    renderer_triangle(cpu);
    renderer_general_compare_planes(gpu, cpu, nth);
    g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-fallbacks"), ==,
                     fallbacks + !host);
    qtest_quit(cpu);
    qtest_quit(gpu);
}

/*
 * A depth-tested quad over a whole 1280x1024 window takes the legacy route
 * as two triangles in 80 tiles each, is held whole for the comparison, and
 * is committed exactly as a host-gpu=off board draws it.
 */
static void test_renderer_legacy_full_window(void)
{
    QTestState *gpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { gpu, cpu };
    uint64_t requests, draws;

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        QTestState *s = states[i];

        gl_make_current_depth(s, 24);
        kona_command_kicked(s, 0x1f3,
                            (uint32_t[]){ 0, 0, 0x05670080, 0x05670080,
                                          0, 0, FB_WIDTH, FB_HEIGHT, 0, 0xd,
                                          0 }, 11);
        kona_command_kicked(s, 0x1ee, (uint32_t[]){ 0, 0, 0 }, 3);
        kona_command(s, 0xe3, (uint32_t[]){ 0, 0, FB_WIDTH, FB_HEIGHT }, 4);
        kona_command(s, 0x25, (uint32_t[]){ 0xbd0 }, 1);
        kona_command(s, 0x1a, (uint32_t[]){ 0x100 }, 1);
        kona_command(s, 0x24, (uint32_t[]){ 0xb71 }, 1);
        kona_command_f(s, 0x63, (float[]){ 0, 0.5f, 1 }, 3);
    }
    requests = renderer_counter(gpu, "x-test-gpu-requests");
    draws = renderer_counter(gpu, "x-test-gpu-draws");
    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        gl_begin(states[i], 7);
        gl_vertex2f(states[i], -1, -1);
        gl_vertex2f(states[i], 1, -1);
        gl_vertex2f(states[i], 1, 1);
        gl_vertex2f(states[i], -1, 1);
        gl_end(states[i]);
    }
    renderer_general_compare_planes(gpu, cpu, 0);
    if (g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-requests"), ==,
                         requests + 160);
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==,
                         draws + 1);
    }
    g_assert_cmpuint(renderer_plane_at(gpu, 0, 0, 0), ==, 0x0080ff);
    g_assert_cmpuint(renderer_plane_at(gpu, 0, FB_WIDTH - 1, FB_HEIGHT - 1),
                     ==, 0x0080ff);
    qtest_quit(cpu);
    qtest_quit(gpu);
}

/*
 * Two thin quads along the diagonals of a whole 1280x1024 window, each one
 * legacy host draw. Their held writes start small and grow across the
 * window, leftwards for the second, in the different orders the host batch
 * and the scalar draw reach them, and each draw is committed exactly as a
 * host-gpu=off board draws it.
 */
static void test_renderer_legacy_diagonals(void)
{
    static const float quads[2][4][2] = {
        { { -1, -1 }, { -0.98f, -1 }, { 1, 1 }, { 0.98f, 1 } },
        { { 0.98f, -1 }, { 1, -1 }, { -0.98f, 1 }, { -1, 1 } },
    };
    QTestState *gpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { gpu, cpu };
    uint64_t draws;

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        QTestState *s = states[i];

        gl_make_current_depth(s, 24);
        kona_command_kicked(s, 0x1f3,
                            (uint32_t[]){ 0, 0, 0x05670080, 0x05670080,
                                          0, 0, FB_WIDTH, FB_HEIGHT, 0, 0xd,
                                          0 }, 11);
        kona_command_kicked(s, 0x1ee, (uint32_t[]){ 0, 0, 0 }, 3);
        kona_command(s, 0xe3, (uint32_t[]){ 0, 0, FB_WIDTH, FB_HEIGHT }, 4);
        kona_command(s, 0x25, (uint32_t[]){ 0xbd0 }, 1);
        kona_command(s, 0x1a, (uint32_t[]){ 0x100 }, 1);
        kona_command(s, 0x24, (uint32_t[]){ 0xb71 }, 1);
        kona_command_f(s, 0x63, (float[]){ 0, 0.5f, 1 }, 3);
    }
    draws = renderer_counter(gpu, "x-test-gpu-draws");
    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        for (unsigned q = 0; q < ARRAY_SIZE(quads); q++) {
            gl_begin(states[i], 7);
            for (unsigned v = 0; v < 4; v++) {
                gl_vertex2f(states[i], quads[q][v][0], quads[q][v][1]);
            }
            gl_end(states[i]);
        }
    }
    renderer_general_compare_planes(gpu, cpu, 0);
    if (g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_assert_cmpuint(renderer_counter(gpu, "x-test-gpu-draws"), ==,
                         draws + 2);
    }
    g_assert_cmpuint(renderer_plane_at(gpu, 0, FB_WIDTH / 2, FB_HEIGHT / 2),
                     ==, 0x0080ff);
    g_assert_cmpuint(renderer_plane_at(gpu, 0, FB_WIDTH / 2, 0), !=,
                     0x0080ff);
    qtest_quit(cpu);
    qtest_quit(gpu);
}

/*
 * Meson runs this binary once per group in
 * sn0-origin2000-kona-3d-test-groups.txt, naming the group's GLib paths with
 * --group-path, so that its groups run at once. Only the cases a group takes
 * are registered. GLib's own -p selects as well, but it reports each path as
 * a separate TAP plan, which Meson rejects. A path names one case or a suite
 * whose cases directly under it all join the group. Without --group-path,
 * every case runs.
 */
static GPtrArray *group_paths;

static void take_group_paths(int *argc, char **argv)
{
    int kept = 1;

    for (int i = 1; i < *argc; i++) {
        if (g_str_equal(argv[i], "--group-path") && i + 1 < *argc) {
            if (!group_paths) {
                group_paths = g_ptr_array_new();
            }
            g_ptr_array_add(group_paths, argv[++i]);
        } else {
            argv[kept++] = argv[i];
        }
    }
    argv[kept] = NULL;
    *argc = kept;
}

static bool in_group(const char *path)
{
    g_autofree char *full = g_strdup_printf("/%s/%s", qtest_get_arch(),
                                            path);
    g_autofree char *suite = g_path_get_dirname(full);

    if (!group_paths) {
        return true;
    }
    for (guint i = 0; i < group_paths->len; i++) {
        const char *taken = g_ptr_array_index(group_paths, i);

        if (g_str_equal(taken, full) || g_str_equal(taken, suite)) {
            return true;
        }
    }
    return false;
}

static void add_case(const char *path, GTestFunc fn)
{
    if (in_group(path)) {
        qtest_add_func(path, fn);
    }
}

static void add_data_case(const char *path, gconstpointer data,
                          GTestDataFunc fn)
{
    if (in_group(path)) {
        qtest_add_data_func(path, data, fn);
    }
}

/* Renderer cases with this flag make host renderer creation unavailable. */
#define RENDERER_CPU (1u << 16)

static unsigned renderer_mode(gconstpointer opaque)
{
    return GPOINTER_TO_UINT(opaque) & ~RENDERER_CPU;
}

static QTestState *renderer_init(gconstpointer opaque)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");

    if (GPOINTER_TO_UINT(opaque) & RENDERER_CPU) {
        qtest_qmp_assert_success(s,
            "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
            "'property':'x-test-gpu-fail-create','value':1}}");
    }
    return s;
}

/*
 * Each draw either completes on the host renderer or falls back to the
 * scalar path. Refused draws use unsupported state and always fall back.
 * Strict Mesa runs require every eligible draw on the host, except where
 * an exact precision check may refuse it.
 */
static void renderer_assert_draws(QTestState *s, gconstpointer opaque,
                                  unsigned eligible, unsigned refused,
                                  bool precision)
{
    uint64_t draws = renderer_counter(s, "x-test-gpu-draws");
    uint64_t fallbacks = renderer_counter(s, "x-test-gpu-fallbacks");

    g_assert_cmpuint(draws + fallbacks, ==, eligible + refused);
    g_assert_cmpuint(draws, <=, eligible);
    if (GPOINTER_TO_UINT(opaque) & RENDERER_CPU) {
        g_assert_cmpuint(draws, ==, 0);
    } else if (!precision && g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_assert_cmpuint(draws, ==, eligible);
    }
}

/*
 * Register modes 0 to modes - 1 of a renderer case, at path templates that
 * spell their suites in full, so scripts/check-qtest-registered.py can see
 * them: host is drawn with the host renderer, cpu with it unavailable.
 */
static void renderer_add_modes(const char *host, const char *cpu,
                               unsigned modes, GTestDataFunc fn)
{
    for (unsigned unavailable = 0; unavailable < 2; unavailable++) {
        for (unsigned mode = 0; mode < modes; mode++) {
            g_autofree char *path = g_strdup_printf(unavailable ? cpu : host,
                                                    mode);

            add_data_case(path,
                          GUINT_TO_POINTER(mode |
                                           (unavailable ? RENDERER_CPU : 0)),
                          fn);
        }
    }
}

static void test_renderer_polygon(gconstpointer opaque)
{
    unsigned mode = renderer_mode(opaque);
    unsigned primitive = mode == 1 ? 8 : mode == 2 ? 9 : 7;
    unsigned count = mode == 2 ? 3 : 4;
    float vertices[4][2] = {
        { 0, 0 }, { 128, 0 }, { 128, 128 }, { 0, 128 },
    };
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    if (mode == 1) {
        vertices[1][0] = 0;
        vertices[1][1] = 128;
        vertices[2][1] = 0;
        vertices[3][0] = 128;
    } else if (mode == 3) {
        vertices[3][0] = -.001;
    }
    gl_begin(s, primitive);
    for (unsigned i = 0; i < count; i++) {
        kona_command_f(s, 0x63, (float[]) { i == 0, 0, i != 0 }, 3);
        gl_vertex2f(s, vertices[i][0], vertices[i][1]);
    }
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     mode == 3 ? 3 : mode == 2 ? 1 : 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    expected_clear(expected, mode == 2 ? 0xffffff : 0x0000ff);
    if (mode == 2) {
        for (unsigned y = 0; y < 128; y++) {
            for (unsigned x = y; x < 128; x++) {
                unsigned offset = ((127 - y) * 128 + x) * 3;

                expected->rgb[offset] = 255;
                expected->rgb[offset + 1] = expected->rgb[offset + 2] = 0;
            }
        }
    }
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_large_polygon(gconstpointer opaque)
{
    unsigned mode = renderer_mode(opaque);
    unsigned count = mode == 0 ? 4 : mode == 4 ? 259 :
                     mode == 5 ? 258 : 5;
    const float vertices[5][2] = {
        { 0, 0 }, { 128, 0 }, { 128, 64 }, { 128, 128 }, { 0, 128 },
    };
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    gl_begin(s, 9);
    for (unsigned i = 0; i < count; i++) {
        unsigned vertex = mode == 0 ? (i < 2 ? i : i + 1) :
                          mode == 2 ? (i ? 5 - i : 0) : i < 5 ? i : 0;
        float x = vertices[vertex][0], y = vertices[vertex][1];

        if (mode == 3 && i == count - 1) {
            x = -.001;
        }
        kona_command_f(s, 0x63, (float[]) { i == 0, 0, i != 0 }, 3);
        gl_vertex2f(s, x, y);
    }
    gl_end(s);
    /* Clipping the fifth vertex adds two intersections to the four inside. */
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     mode == 4 ? 0 : mode == 3 ? 4 : count - 2);
    renderer_assert_draws(s, opaque, mode != 4, mode == 4, false);
    expected_clear(expected, 0xff0000);
    assert_window_matches(s, expected);
    if (mode == 4) {
        /* More triangles than the host batch stages is a host limit. */
        g_assert_cmpuint(renderer_fallback_reason(s, "host-staging"), ==, 1);
        kona_command_f(s, 0x63, (float[]) { 1, 0, 0 }, 3);
        renderer_triangle(s);
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 1);
        renderer_assert_draws(s, opaque, 1, 1, false);
        assert_window_matches(s, expected);
    }
    qtest_quit(s);
}

static void test_renderer_scissor(gconstpointer opaque)
{
    static const int32_t boxes[][4] = {
        { 16, 32, 64, 48 }, { -16, -32, 64, 48 },
        { 128, 0, 1, 128 }, { 0, 0, 0, 128 }, { 0, 0, 128, 0 },
        { 64, 64, INT32_MAX, INT32_MAX },
        { INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX }, { -1, -1, 1, 1 },
    };
    unsigned mode = renderer_mode(opaque);
    const int32_t *box = boxes[mode];
    uint32_t words[4];
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    for (unsigned i = 0; i < 4; i++) {
        words[i] = box[i];
    }
    kona_command(s, 0xc, words, 4);
    kona_command(s, 0x24, (uint32_t[]) { 0xc11 }, 1);
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    expected_clear(expected, 0xffffff);
    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 128; x++) {
            if ((int64_t)x >= box[0] && (int64_t)y >= box[1] &&
                (int64_t)x < (int64_t)box[0] + box[2] &&
                (int64_t)y < (int64_t)box[1] + box[3]) {
                unsigned offset = ((127 - y) * 128 + x) * 3;

                expected->rgb[offset] = 255;
                expected->rgb[offset + 1] = expected->rgb[offset + 2] = 0;
            }
        }
    }
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_viewport(gconstpointer opaque)
{
    static const int32_t viewports[][4] = {
        { 0, 0, 256, 256 }, { 16, 32, 64, 48 }, { -32, -16, 192, 160 },
        { 128, 0, 128, 128 }, { 0, 0, 0, 128 },
        { 0, 0, 32768, 32768 }, { 0, 0, 16384, 16384 }, { 0, 0, 256, 128 },
    };
    unsigned mode = renderer_mode(opaque);
    const int32_t *vp = viewports[mode];
    uint32_t words[4];
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    if (mode == 7) {
        kona_command_kicked(s, 0x1f3,
            (uint32_t[]) { PROBE_X, PROBE_Y, 0x05670100, 0x05670080,
                           PROBE_X, PROBE_Y, 256, 128, 0, 0xd, 0 }, 11);
    }
    for (unsigned i = 0; i < 4; i++) {
        words[i] = vp[i];
    }
    kona_command(s, 0xe3, words, 4);
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     mode == 5 ? 0 : mode == 7 ? 4 : 2);
    renderer_assert_draws(s, opaque, mode != 5, mode == 5, false);
    expected_clear(expected, 0xffffff);
    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 128; x++) {
            if ((int64_t)x >= vp[0] && (int64_t)y >= vp[1] &&
                (int64_t)x < (int64_t)vp[0] + vp[2] &&
                (int64_t)y < (int64_t)vp[1] + vp[3]) {
                unsigned offset = ((127 - y) * 128 + x) * 3;

                expected->rgb[offset] = 255;
                expected->rgb[offset + 1] = expected->rgb[offset + 2] = 0;
            }
        }
    }
    assert_window_matches(s, expected);
    if (mode == 7) {
        g_autofree uint8_t *frame = kona_screendump(s);
        const uint8_t *pixel = frame +
            ((FB_HEIGHT - 1 - PROBE_Y - 64) * FB_WIDTH + PROBE_X + 200) * 3;

        g_assert_cmpuint(pixel[0], ==, 255);
        g_assert_cmpuint(pixel[1], ==, 0);
        g_assert_cmpuint(pixel[2], ==, 0);
    }
    qtest_quit(s);
}

static void test_renderer_tiles(gconstpointer opaque)
{
    static const struct {
        int32_t x, y;
        unsigned width, height, requests;
    } windows[] = {
        { PROBE_X, PROBE_Y, 256, 256, 8 },
        { PROBE_X, PROBE_Y, 257, 193, 12 },
        { PROBE_X, PROBE_Y, 129, 1, 4 },
        { PROBE_X, PROBE_Y, 1, 129, 4 },
        { PROBE_X, PROBE_Y, 64, 63, 2 },
        { FB_WIDTH - 65, FB_HEIGHT - 63, 129, 129, 2 },
        { -17, -33, 193, 193, 8 },
        { 0, 0, FB_WIDTH, FB_HEIGHT, 160 },
        { PROBE_X, PROBE_Y, 257, 193, 12 },
    };
    unsigned mode = renderer_mode(opaque);
    int32_t ox = windows[mode].x, oy = windows[mode].y;
    unsigned width = windows[mode].width, height = windows[mode].height;
    QTestState *s = renderer_init(opaque);
    g_autofree uint8_t *expected = NULL, *frame = NULL;

    renderer_fixture(s);
    kona_command_kicked(s, 0x1f3,
        (uint32_t[]) { ox, oy, 0x05670000 | width, 0x05670000 | height,
                       ox, oy, width, height, 0, 0xd, 0 }, 11);
    kona_command_kicked(s, 0x1ee, (uint32_t[]) { ox, oy, 0 }, 3);
    kona_command(s, 0xe3, (uint32_t[]) { 0, 0, width, height }, 4);
    if (mode == 8) {
        kona_command(s, 0xc, (uint32_t[]) { 120, 70, 70, 80 }, 4);
        kona_command(s, 0x24, (uint32_t[]) { 0xc11 }, 1);
    }
    expected = kona_screendump(s);
    kona_command_f(s, 0x63, (float[]) { 17 / 255.0f, 63 / 255.0f,
                                      191 / 255.0f }, 3);
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     windows[mode].requests);
    renderer_assert_draws(s, opaque, 1, 0, false);
    frame = kona_screendump(s);
    for (unsigned y = 0; y < FB_HEIGHT; y++) {
        for (unsigned x = 0; x < FB_WIDTH; x++) {
            int64_t lx = (int64_t)x - ox, ly = (int64_t)y - oy;
            unsigned offset = ((FB_HEIGHT - 1 - y) * FB_WIDTH + x) * 3;
            uint8_t *pixel = expected + offset;

            if (lx >= 0 && lx < width && ly >= 0 && ly < height &&
                (mode != 8 || (lx >= 120 && lx < 190 &&
                                ly >= 70 && ly < 150))) {
                pixel[0] = 17;
                pixel[1] = 63;
                pixel[2] = 191;
            }
            if (memcmp(frame + offset, pixel, 3)) {
                g_error("tiled window pixel (%u,%u) shows %02x%02x%02x, "
                        "expected %02x%02x%02x", x, y,
                        frame[offset], frame[offset + 1], frame[offset + 2],
                        pixel[0], pixel[1], pixel[2]);
            }
        }
    }
    qtest_quit(s);
}

static void test_renderer_culling(gconstpointer opaque)
{
    static const uint32_t faces[] = {
        0x405, 0x404, 0x408, 0x405, 0x404, 0x405, 0x404, 0x405,
    };
    static const bool visible[] = { true, false, false, false,
                                   true, false, true, true };
    unsigned mode = renderer_mode(opaque);
    uint32_t winding = mode == 3 || mode == 4 || mode == 7 ? 0x900 : 0x901;
    const float vertices[4][2] = {
        { 0, 0 }, { 128, 0 }, { 128, 128 }, { 0, 128 },
    };
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    kona_command(s, 0xb6, &faces[mode], 1);
    kona_command(s, 0xba, &winding, 1);
    kona_command(s, 0x24, (uint32_t[]) { 0xb44 }, 1);
    gl_begin(s, 7);
    for (unsigned i = 0; i < 4; i++) {
        unsigned vertex = mode >= 5 && i ? 4 - i : i;

        gl_vertex2f(s, vertices[vertex][0], vertices[vertex][1]);
    }
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    expected_clear(expected, visible[mode] ? 0xff0000 : 0xffffff);
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_colour_mask(gconstpointer opaque)
{
    unsigned mode = renderer_mode(opaque), mask = mode & 15;
    bool direct = mode & 16;
    const uint8_t old[3] = { direct ? 47 : 255, direct ? 89 : 255,
                             direct ? 123 : 255 };
    const uint8_t colour[3] = { 17, 63, 191 };
    uint32_t words[4], rgb = 0;
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    if (direct) {
        kona_command_f(s, 0x1d, (float[]) { 47 / 255.0f, 89 / 255.0f,
                                           123 / 255.0f, 1 }, 4);
        kona_command(s, 0x1a, (uint32_t[]) { 0x4000 }, 1);
    }
    for (unsigned i = 0; i < 4; i++) {
        words[i] = mask & (1u << i);
        if (i < 3) {
            rgb = (rgb << 8) | (words[i] ? colour[i] : old[i]);
        }
    }
    kona_command(s, 0x21, words, 4);
    kona_command_f(s, 0x63, (float[]) { 17 / 255.0f, 63 / 255.0f,
                                      191 / 255.0f }, 3);
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    expected_clear(expected, rgb);
    assert_window_matches(s, expected);
    /* A fully masked indexed pixel must still follow its palette entry. */
    kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    if (!direct && !(mask & 7)) {
        expected_clear(expected, 0x326496);
    }
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_logic_op(gconstpointer opaque)
{
    static const uint8_t values[16] = {
        0x00, 0xc0, 0x0c, 0xcc, 0x30, 0xf0, 0x3c, 0xfc,
        0x03, 0xc3, 0x0f, 0xcf, 0x33, 0xf3, 0x3f, 0xff,
    };
    unsigned mode = renderer_mode(opaque), op = mode & 15;
    unsigned variant = mode / 16;
    uint32_t rgb = (variant == 4 ? 0xcc : values[op]) * 0x010101;
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    if (!variant) {
        kona_command(s, 1, (uint32_t[]) { 3, 0x22, 960, 960, 960 }, 5);
    } else {
        kona_command_f(s, 0x1d, (float[]) { 240 / 255.0f, 240 / 255.0f,
                                           240 / 255.0f, 1 }, 4);
        kona_command(s, 0x1a, (uint32_t[]) { 0x4000 }, 1);
    }
    kona_command(s, 0x2e, (uint32_t[]) { 0x1500 + op }, 1);
    if (variant == 1) {
        kona_command(s, 0x21, (uint32_t[]) { 1, 0, 1, 0 }, 4);
        rgb = (rgb & 0xff00ff) | 0x00f000;
    }
    if (variant == 2 || variant == 3) {
        kona_command(s, 0xcb, (uint32_t[]) { 0, 0 }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbe2 }, 1);
    }
    if (variant == 3) {
        kona_command(s, 0xe7, (uint32_t[]) { 0xbf1 }, 1);
    } else {
        kona_command(s, 0x24,
                     (uint32_t[]) { variant == 4 ? 0xbf1 : 0xbf2 }, 1);
    }
    kona_command_f(s, 0x63, (float[]) { 204 / 255.0f, 204 / 255.0f,
                                      204 / 255.0f }, 3);
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    expected_clear(expected, rgb);
    assert_window_matches(s, expected);
    kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_alpha_test(gconstpointer opaque)
{
    static const struct {
        float alpha, ref;
        int relation;
    } cases[] = {
        { 1, 0.999f, 0 }, { 1, 0.997f, 1 },
        { 0.5f, 0.501f, 0 }, { 0.5f, 0.51f, -1 },
        { 0, 0, 0 }, { -1, 0, 0 }, { 2, 1, 0 }, { 0.25f, 0.5f, -1 },
    };
    static const uint8_t passes[] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    unsigned mode = renderer_mode(opaque), func = mode & 7;
    unsigned variant = mode / 8;
    bool kept = passes[func] & (1 << (1 - cases[variant].relation));
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);
    uint32_t args[] = { 0x200 + func, fbits(cases[variant].ref) };

    renderer_fixture(s);
    kona_command(s, 0xca, args, ARRAY_SIZE(args));
    kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
    /* Only the quad's last vertex supplies the flat fragment alpha. */
    kona_command_f(s, 0x6b, (float[]) { 1, 0, 0, 1 }, 4);
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    kona_command_f(s, 0x6b,
                   (float[]) { 1, 0, 0, cases[variant].alpha }, 4);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    expected_clear(expected, kept ? 0xff0000 : 0xffffff);
    assert_window_matches(s, expected);
    kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    expected_clear(expected, kept ? 0xff0000 : 0x326496);
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_blend(gconstpointer opaque)
{
    static const struct {
        uint32_t src, dst, equation;
        float rgba[4];
        uint32_t rgb;
    } cases[] = {
        { 0x302, 0x303, 0x8006, { 0, 1, 1, 0.25f }, 0xbfa040 },
        { 1, 1, 0x8006, { 0, 0.25f, 1, 1 }, 0xffc0ff },
        { 1, 1, 0x800a, { 0.5f, 0.25f, 1, 1 }, 0x0000ff },
        { 1, 1, 0x800b, { 0.5f, 0.25f, 1, 1 }, 0x7f4000 },
        { 0, 0, 0x8007, { 0.5f, 1, 1, 1 }, 0x808000 },
        { 0, 0, 0x8008, { 0.5f, 0.25f, 1, 1 }, 0xff80ff },
        { 0x304, 0x305, 0x8006, { 0, 0, 1, 0 }, 0x0000ff },
        { 0x308, 1, 0x8006, { 0, 0, 1, 0.25f }, 0xff8000 },
        { 0x8001, 0x8004, 0x8006, { 1, 1, 1, 1 }, 0xff80ff },
        { 0x306, 0x300, 0x8006, { 0.5f, 0.5f, 0.5f, 1 }, 0xff8100 },
        { 0x306, 0, 0x8006, { 0.5f, 0.5f, 0.5f, 1 }, 0x804000 },
        { 0x307, 0, 0x8006, { 0.5f, 0.5f, 0.5f, 1 }, 0x004080 },
        { 0, 0x300, 0x8006, { 0.5f, 0.5f, 0.5f, 1 }, 0x804000 },
        { 0, 0x301, 0x8006, { 0.5f, 0.5f, 0.5f, 1 }, 0x7f4000 },
        { 0x8003, 0, 0x8006, { 1, 1, 1, 1 }, 0x808080 },
        { 0x8004, 0, 0x8006, { 1, 1, 1, 1 }, 0x808080 },
        { 0x8001, 0, 0x8006, { 1, 1, 1, 1 }, 0x8040ff },
        { 0x8002, 0, 0x8006, { 1, 1, 1, 1 }, 0x80bf00 },
    };
    unsigned mode = renderer_mode(opaque);
    unsigned variant = mode / ARRAY_SIZE(cases);
    unsigned index = mode % ARRAY_SIZE(cases);
    uint32_t rgb = cases[index].rgb;
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    if (!variant) {
        kona_command(s, 1, (uint32_t[]) { 3, 0x22, 1020, 512, 0 }, 5);
    } else {
        kona_command_f(s, 0x1d, (float[]) { 1, 0.5f, 0, 1 }, 4);
        kona_command(s, 0x1a, (uint32_t[]) { 0x4000 }, 1);
    }
    if (variant == 2) {
        kona_command(s, 0x21, (uint32_t[]) { 1, 0, 1, 0 }, 4);
        rgb = (rgb & 0xff00ff) | 0x008000;
    }
    kona_command(s, 0xcb,
                 (uint32_t[]) { cases[index].src, cases[index].dst }, 2);
    kona_command(s, 0xe7, &cases[index].equation, 1);
    kona_command_f(s, 0xe6, (float[]) { 0.5f, 0.25f, 1, 0.5f }, 4);
    kona_command(s, 0x24, (uint32_t[]) { 0xbe2 }, 1);
    kona_command_f(s, 0x6b, cases[index].rgba, 4);
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    expected_clear(expected, rgb);
    assert_window_matches(s, expected);
    kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_smooth(gconstpointer opaque)
{
    unsigned input = renderer_mode(opaque);
    bool gpu = input == 13;
    unsigned mode = gpu ? 1 : input;
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    static const float colours[4][4] = {
        { 1, 0, 0, 0 }, { 0, 1, 0, 0.5f }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 },
    };
    static const float positions[4][2] = {
        { 0, 0 }, { 128, 0 }, { 128, 128 }, { 0, 128 },
    };
    const double inverse_w[4] = { 1, mode == 1 ? 2 : 1,
                                 mode == 1 ? 4 : 1, 1 };
    QTestState *s;
    g_autofree Expected *expected = g_new(Expected, 1);

    if (gpu && !node) {
        qtest_prereq_missing("SGI_RENDER_NODE",
                             "QEMU_TEST_SGI_RENDER_NODE is not set");
        return;
    }
    s = gpu ? qtest_initf(MACHINE_ARGS " -global sgi-kona.host-gpu=on"
                          " -display egl-headless,rendernode=%s", node) :
              renderer_init(opaque);
    renderer_fixture(s);
    kona_command(s, 0xc0, (uint32_t[]) { 0x1d01 }, 1);
    if (mode == 4) {
        kona_command(s, 0xca, (uint32_t[]) { 0x206, fbits(0.5f) }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
    } else if (mode == 5) {
        kona_command(s, 0xcb, (uint32_t[]) { 0x302, 0x303 }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbe2 }, 1);
    } else if (mode == 6) {
        kona_command(s, 0x21, (uint32_t[]) { 1, 0, 1, 0 }, 4);
    } else if (mode == 7) {
        kona_command(s, 0x2e, (uint32_t[]) { 0x1506 }, 1);
        kona_command(s, 0x24, (uint32_t[]) { 0xbf2 }, 1);
    } else if (mode == 8) {
        kona_command(s, 0xc, (uint32_t[]) { 32, 16, 64, 80 }, 4);
        kona_command(s, 0x24, (uint32_t[]) { 0xc11 }, 1);
    } else if (mode == 9) {
        kona_command(s, 0xb6, (uint32_t[]) { 0x404 }, 1);
        kona_command(s, 0x24, (uint32_t[]) { 0xb44 }, 1);
    }
    gl_begin(s, mode < 10 ? 4 : mode == 10 ? 7 : mode == 11 ? 9 : 8);
    for (unsigned v = 0; v < (mode < 10 ? 3 : 4); v++) {
        unsigned index = mode == 2 && v ? 3 - v :
                         mode == 12 && v >= 2 ? 5 - v : v;
        unsigned position = mode < 10 && index == 2 ? 3 : index;
        float colour[4], w = 1 / inverse_w[index];

        memcpy(colour, colours[index], sizeof(colour));
        if (mode == 3) {
            for (unsigned c = 0; c < 3; c++) {
                colour[c] = colour[c] ? 2 : -1;
            }
        }
        kona_command_f(s, 0x6b, colour, 4);
        kona_command_f(s, 0x94,
                       (float[]) { positions[position][0] * w,
                                   positions[position][1] * w, 0, w }, 4);
    }
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==,
                     mode < 10 ? 1 : 2);
    /* A host precision refusal must preserve the exact software image. */
    renderer_assert_draws(s, opaque, 1, 0, true);
    for (unsigned palette = 0; palette < 2; palette++) {
        expected_clear(expected, palette ? 0x326496 : 0xffffff);
        for (unsigned y = 0; y < 128; y++) {
            for (unsigned x = 0; x < 128; x++) {
                /* Twice the barycentric numerators at this pixel centre. */
                double weights[3] = { 254 - 2 * (double)(x + y),
                                      2 * x + 1, 2 * y + 1 };
                double sum = 0, alpha;
                uint8_t *pixel = expected->rgb + ((127 - y) * 128 + x) * 3;

                if (mode >= 10) {
                    double u = (x + 0.5) / 128, v = (y + 0.5) / 128;

                    pixel[0] = floor((1 - u) * 255 + 0.5);
                    pixel[1] = floor(fabs(u - v) * 255 + 0.5);
                    pixel[2] = floor(v * 255 + 0.5);
                    continue;
                }
                if (x + y >= 127 || mode == 9 ||
                    (mode == 8 && (x < 32 || x >= 96 || y < 16 || y >= 96))) {
                    continue;
                }
                for (unsigned c = 0; c < 3; c++) {
                    weights[c] *= inverse_w[c];
                    sum += weights[c];
                }
                alpha = floor((weights[1] * 0.5 + weights[2]) / sum *
                               255 + 0.5);
                if (mode == 4 && alpha < 128) {
                    continue;
                }
                for (unsigned c = 0; c < 3; c++) {
                    double value = floor(weights[c] / sum * 255 + 0.5);

                    if (mode == 5) {
                        value = floor(value * alpha / 255 + 255 - alpha + 0.5);
                    } else if (mode == 6 && c == 1) {
                        value = 255;
                    } else if (mode == 7) {
                        value = 255 - value;
                    }
                    pixel[c] = value;
                }
            }
        }
        assert_window_matches(s, expected);
        kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    }
    qtest_quit(s);
}

static void test_renderer_depth(gconstpointer opaque)
{
    static const uint8_t passes[] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    unsigned mode = renderer_mode(opaque), func = mode & 7;
    unsigned variant = mode / 8;
    int relation = variant == 1 ? 0 : variant == 2 || variant == 6 ? 1 : -1;
    bool kept = variant == 5 || (passes[func] & (1 << (1 - relation)));
    uint32_t old = variant == 1 ? 0x800000 :
                   variant == 2 ? 0x400000 : 0x9fffff;
    uint32_t incoming = variant == 6 ? 0xbfffff : 0x800000;
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    if (variant == 1 || variant == 2) {
        kona_command_d(s, 0x1f, (double[]) { variant == 1 ? 0.5 : 0.25 }, 1);
        kona_command(s, 0x1a, (uint32_t[]) { 0x100 }, 1);
    } else if (variant == 3) {
        kona_command(s, 0x22, (uint32_t[]) { 0 }, 1);
    } else if (variant == 4) {
        kona_command(s, 0x21, (uint32_t[]) { 0, 0, 0, 0 }, 4);
    } else if (variant == 5) {
        kona_command(s, 1, (uint32_t[]) { 0, 12, 0, 0, 0, 0, 0, 0, 0, 0 }, 10);
    } else if (variant == 6) {
        kona_command_d(s, 0xcf, (double[]) { 0.75, 0.75 }, 2);
    } else if (variant == 7) {
        kona_command(s, 0xca, (uint32_t[]) { 0x200, F0 }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
        kept = false;
    }
    kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
    kona_command(s, 0xce, (uint32_t[]) { 0x200 + func }, 1);
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    g_assert_cmphex(stencil_colour_or_depth(s, 5), ==,
                    kept && variant != 3 && variant != 5 ? incoming : old);
    g_assert_cmphex(stencil_colour_or_depth(s, 6), ==, 0x5a);
    expected_clear(expected, kept && variant != 4 ? 0xff0000 : 0xffffff);
    assert_window_matches(s, expected);
    kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    expected_clear(expected, kept && variant != 4 ? 0xff0000 : 0x326496);
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_stencil(gconstpointer opaque)
{
    static const uint32_t ops[] = { 0x1e00, 0, 0x1e01, 0x1e02, 0x1e03, 0x150a };
    static const uint32_t results[] = { 0xa5, 0, 0x33, 0xa6, 0xa4, 0x5a };
    unsigned mode = renderer_mode(opaque), op = mode % 6;
    unsigned outcome = mode / 6 % 3, variant = mode / 18;
    uint32_t mask = variant == 1 ? 15 : 255;
    uint32_t value = (0xa5 & ~mask) | (results[op] & mask);
    uint32_t selected[3] = { 0x1e00, 0x1e00, 0x1e00 };
    bool colour = outcome == 2 && variant != 2;
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    stencil_config(s, 8);
    stencil_clear(s, 0xa5, 255);
    kona_command(s, 0x20, &mask, 1);
    stencil_function(s, outcome == 0 ? 0x200 : 0x207, 0x33, 255);
    selected[outcome] = ops[op];
    stencil_operations(s, selected[0], selected[1], selected[2]);
    kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
    kona_command(s, 0xce, (uint32_t[]) { outcome == 1 ? 0x200 : 0x207 }, 1);
    if (variant == 2) {
        kona_command(s, 0x21, (uint32_t[]) { 0, 0, 0, 0 }, 4);
    }
    gl_begin(s, 7);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 128, 0);
    gl_vertex2f(s, 128, 128);
    gl_vertex2f(s, 0, 128);
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 2);
    renderer_assert_draws(s, opaque, 1, 0, false);
    g_assert_cmphex(stencil_colour_or_depth(s, 6), ==, value);
    g_assert_cmphex(stencil_colour_or_depth(s, 5), ==,
                    outcome == 2 ? 0x800000 : 0x9fffff);
    expected_clear(expected, colour ? 0xff0000 : 0xffffff);
    assert_window_matches(s, expected);
    kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    expected_clear(expected, colour ? 0xff0000 : 0x326496);
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_depth_gradient(gconstpointer opaque)
{
    unsigned mode = renderer_mode(opaque);
    static const float positions[3][2] = { { 0, 0 }, { 128, 0 }, { 0, 128 } };
    static const unsigned probes[4][2] = {
        { 32, 16 }, { 64, 16 }, { 16, 64 }, { 96, 96 },
    };
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);
    uint32_t depth[4], stencil[4];
    double offset = 0;

    renderer_fixture(s);
    stencil_config(s, 8);
    stencil_function(s, 0x207, 0, 255);
    stencil_operations(s, 0x1e02, 0x1e02, 0x1e02);
    kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
    kona_command(s, 0xce, (uint32_t[]) { 0x201 }, 1);
    if (mode == 1) {
        kona_command_d(s, 0xcf, (double[]) { 1, 0 }, 2);
    } else if (mode == 2) {
        kona_command(s, 0xc0, (uint32_t[]) { 0x1d01 }, 1);
        kona_command(s, 0xca, (uint32_t[]) { 0x206, fbits(0.5f) }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
    } else if (mode == 3) {
        kona_command(s, 0xc, (uint32_t[]) { 32, 16, 64, 80 }, 4);
        kona_command(s, 0x24, (uint32_t[]) { 0xc11 }, 1);
    }
    if (mode >= 4) {
        kona_command(s, 0x24, (uint32_t[]) { mode == 9 ? 0x2a02 : 0x8037 }, 1);
        if (mode == 4 || mode == 5) {
            float factor = mode == 4 ? 1 : -1;

            kona_command_f(s, 0x168, (float[]) { factor, 0 }, 2);
            offset = factor / 128.0;
        } else if (mode == 6 || mode == 9) {
            kona_command_f(s, 0x168, (float[]) { 0, 4194304 }, 2);
            offset = mode == 6 ? 4194304.0 / 0xffffff : 0;
        } else {
            float bias = mode == 7 ? 0.125f : mode == 8 ? -0.125f :
                         mode == 10 ? 1 : -1;

            kona_command_f(s, 0x11b, (float[]) { 0, bias }, 2);
            offset = bias;
        }
    }
    gl_begin(s, 4);
    for (unsigned v = 0; v < 3; v++) {
        float w = 1 << v;

        kona_command_f(s, 0x6b, (float[]) { 1, 0, 0, v * 0.5f }, 4);
        kona_command_f(s, 0x94,
                       (float[]) { positions[v][0] * w, positions[v][1] * w,
                                   (1 - (float)v) * w, w }, 4);
    }
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 1);
    renderer_assert_draws(s, opaque, 1, 0, false);
    expected_clear(expected, 0xffffff);
    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 128; x++) {
            double z = ((x + 0.5) * 0.5 + y + 0.5) / 128;
            double b0 = 1 - (x + y + 1.0) / 128;
            double b1 = (x + 0.5) / 256, b2 = (y + 0.5) / 512;
            double alpha = floor((b1 * 0.5 + b2) / (b0 + b1 + b2) *
                                   255 + 0.5);
            bool covered = x + y < 127 && (mode != 2 || alpha >= 128) &&
                (mode != 3 || (x >= 32 && x < 96 && y >= 16 && y < 96));
            double window = CLAMP((mode == 1 ? 1 - z : z) + offset, 0, 1);
            uint32_t zq = floor(window * 0xffffff + 0.5);
            bool kept = covered && zq < 0x9fffff;
            uint8_t *pixel = expected->rgb + ((127 - y) * 128 + x) * 3;

            if (kept) {
                pixel[0] = 255;
                pixel[1] = pixel[2] = 0;
            }
            for (unsigned p = 0; p < ARRAY_SIZE(probes); p++) {
                if (x == probes[p][0] && y == probes[p][1]) {
                    depth[p] = kept ? zq : 0x9fffff;
                    stencil[p] = covered ? 0x5b : 0x5a;
                }
            }
        }
    }
    assert_window_matches(s, expected);
    for (unsigned p = 0; p < ARRAY_SIZE(probes); p++) {
        qtest_qmp_assert_success(s,
            "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
            "'property':'x-test-pixel-x','value':%u}}", PROBE_X + probes[p][0]);
        qtest_qmp_assert_success(s,
            "{'execute':'qom-set','arguments':{'path':'/machine/module0/kona0',"
            "'property':'x-test-pixel-y','value':%u}}", PROBE_Y + probes[p][1]);
        for (unsigned plane = 5; plane <= 6; plane++) {
            QDict *reply;

            qtest_qmp_assert_success(s,
                "{'execute':'qom-set','arguments':"
                "{'path':'/machine/module0/kona0',"
                "'property':'x-test-plane','value':%u}}", plane);
            reply = qtest_qmp(s,
                "{'execute':'qom-get','arguments':"
                "{'path':'/machine/module0/kona0',"
                "'property':'x-test-plane-word'}}");
            g_assert_cmphex(qdict_get_int(reply, "return"), ==,
                            plane == 5 ? depth[p] : stencil[p]);
            qobject_unref(reply);
        }
    }
    qtest_quit(s);
}

static void test_renderer_volume_clip(gconstpointer opaque)
{
    unsigned mode = renderer_mode(opaque);
    float positions[4][3] = {
        { 0, 0, 0 }, { 128, 0, 0 }, { 128, 128, 0 }, { 0, 128, 0 },
    };
    unsigned count = mode >= 6 ? 3 : 4;
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    if (mode == 0) {
        positions[0][0] = positions[3][0] = -64;
    } else if (mode == 1) {
        positions[1][0] = positions[2][0] = 192;
    } else if (mode == 2) {
        positions[0][1] = positions[1][1] = -64;
    } else if (mode == 3) {
        positions[2][1] = positions[3][1] = 192;
    } else if (mode == 4 || mode == 5) {
        positions[0][2] = positions[3][2] = mode == 4 ? 2 : -2;
    } else {
        positions[0][0] = positions[0][1] = -128;
        positions[1][0] = 384;
        positions[1][1] = -128;
        positions[2][0] = -128;
        positions[2][1] = 384;
        if (mode == 7) {
            kona_command(s, 0xc0, (uint32_t[]) { 0x1d01 }, 1);
        } else if (mode == 9) {
            positions[0][0] = positions[1][0] = positions[2][0] = 256;
        }
    }
    gl_begin(s, count == 3 ? 4 : 7);
    for (unsigned v = 0; v < count; v++) {
        float w = mode == 8 ? 1 << v : 1;

        kona_command_f(s, 0x63, (float[]) {
            mode == 7 ? v == 0 : v != count - 1,
            mode == 7 && v == 1,
            mode == 7 ? v == 2 : v == count - 1,
        }, 3);
        kona_command_f(s, 0x94, (float[]) {
            positions[v][0] * w, positions[v][1] * w,
            positions[v][2] * w, w,
        }, 4);
    }
    gl_end(s);
    if (mode == 9) {
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), ==, 0);
    } else {
        g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), >, 0);
    }
    renderer_assert_draws(s, opaque, mode != 9, mode == 9, false);
    expected_clear(expected, 0xffffff);
    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 128; x++) {
            uint8_t *pixel = expected->rgb + ((127 - y) * 128 + x) * 3;

            if (mode == 9 || ((mode == 4 || mode == 5) && x < 64)) {
                continue;
            }
            if (mode == 7) {
                double b1 = (x + 128.5) / 512, b2 = (y + 128.5) / 512;

                pixel[0] = floor((1 - b1 - b2) * 255 + 0.5);
                pixel[1] = floor(b1 * 255 + 0.5);
                pixel[2] = floor(b2 * 255 + 0.5);
            } else {
                pixel[0] = pixel[1] = 0;
                pixel[2] = 255;
            }
        }
    }
    assert_window_matches(s, expected);
    qtest_quit(s);
}

static void test_renderer_user_clip(gconstpointer opaque)
{
    unsigned mode = renderer_mode(opaque);
    unsigned plane = mode < 6 ? mode : 0;
    static const float positions[4][2] = {
        { 0, 0 }, { 128, 0 }, { 128, 128 }, { 0, 128 },
    };
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);
    uint32_t equation[9] = {
        0x3000 + plane, 0x3ff00000, 0, 0, 0, 0, 0,
        mode == 10 ? 0xc0680000 : mode == 11 ? 0xc0502000 : 0xc0500000, 0,
    };

    renderer_fixture(s);
    kona_command(s, 0xb4, equation, ARRAY_SIZE(equation));
    kona_command(s, 0x24, (uint32_t[]) { 0x3000 + plane }, 1);
    if (mode == 6) {
        kona_command(s, 0xb4,
                     (uint32_t[]) { 0x3005, 0, 0, 0x3ff00000, 0, 0, 0,
                                    0xc0400000, 0 }, 9);
        kona_command(s, 0x24, (uint32_t[]) { 0x3005 }, 1);
    } else if (mode == 9) {
        kona_command(s, 0xc0, (uint32_t[]) { 0x1d01 }, 1);
    }
    gl_begin(s, 7);
    for (unsigned v = 0; v < 4; v++) {
        unsigned index = mode == 7 ? (4 - v) % 4 : v;
        float x = positions[index][0], y = positions[index][1];
        float w = mode == 8 ? 1 << v : 1;

        if (mode == 9) {
            kona_command_f(s, 0x63, (float[]) { x / 128, y / 128, 0 }, 3);
        }
        kona_command_f(s, 0x94,
                       (float[]) { x * w, y * w, 0, w }, 4);
    }
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), >, 0);
    renderer_assert_draws(s, opaque, 1, 0, false);
    for (unsigned palette = 0; palette < 2; palette++) {
        expected_clear(expected, palette ? 0x326496 : 0xffffff);
        for (unsigned y = 0; y < 128; y++) {
            for (unsigned x = 64; x < 128; x++) {
                uint8_t *pixel = expected->rgb + ((127 - y) * 128 + x) * 3;

                if (mode == 10 || (mode == 6 && y < 32)) {
                    continue;
                }
                pixel[0] = mode == 9 ? floor((x + 0.5) * 255 / 128 + 0.5) :
                                      255;
                pixel[1] = mode == 9 ? floor((y + 0.5) * 255 / 128 + 0.5) : 0;
                pixel[2] = 0;
            }
        }
        assert_window_matches(s, expected);
        kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    }
    qtest_quit(s);
}

static const uint32_t renderer_texture_pixels[4] = {
    0xff000000, 0x00ff0055, 0x0000ffaa, 0xffffffff,
};

static void renderer_texture_sample(double u, double v, bool linear,
                                      double texel[4])
{
    double sx = (u - floor(u)) * 2 - (linear ? 0.5 : 0);
    double sy = (v - floor(v)) * 2 - (linear ? 0.5 : 0);
    int ix = floor(sx), iy = floor(sy);
    double ax = sx - ix, ay = sy - iy;

    memset(texel, 0, 4 * sizeof(*texel));
    for (unsigned y = 0; y < (linear ? 2 : 1); y++) {
        for (unsigned x = 0; x < (linear ? 2 : 1); x++) {
            unsigned at = ((iy + y + 2) % 2) * 2 + (ix + x + 2) % 2;
            uint32_t pixel = renderer_texture_pixels[at];
            double weight = linear ? (x ? ax : 1 - ax) * (y ? ay : 1 - ay) : 1;

            for (unsigned c = 0; c < 4; c++) {
                texel[c] += ((pixel >> (24 - c * 8)) & 255) / 255.0 * weight;
            }
        }
    }
}

/* Load and enable the 2x2 texture, in an environment from the table. */
static void renderer_texture_bind(QTestState *s, bool linear,
                                  unsigned environment)
{
    static const uint32_t environments[4] = { 0x1e01, 0x2100, 0x2101, 0xbe2 };
    uint32_t h[20], record[22] = { 0x43, 0x101010, 256u << 10 };

    memcpy(h, texture_header, sizeof(h));
    kona_command(s, 0x174, h, ARRAY_SIZE(h));
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, renderer_texture_pixels[k]);
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    kona_command(s, 0x1d5,
                 (uint32_t[]) { 0, (2u << 28) | (41 << 12),
                                (0x30 << 14) | (linear ? 36 : 0), 0x11, 1 },
                 5);
    kona_command(s, 0x179, record, ARRAY_SIZE(record));
    kona_command(s, 0x24, (uint32_t[]) { 0xde1 }, 1);
    kona_command(s, 0x12,
                 (uint32_t[]) { 0x2200, environments[environment], 0, 0, 0 },
                 5);
    kona_command(s, 0x11,
                 (uint32_t[]) { 0x2201, fbits(0.25f), fbits(0.5f),
                                fbits(0.75f), fbits(0.25f) }, 5);
}

static void test_renderer_texture(gconstpointer opaque)
{
    unsigned mode = renderer_mode(opaque);
    static const float positions[4][2] = {
        { 0, 0 }, { 128, 0 }, { 128, 128 }, { 0, 128 },
    };
    unsigned environment = mode == 4 ? 1 : mode == 5 ? 2 : mode == 6 ? 3 :
                           mode >= 7 ? 1 : 0;
    QTestState *s = renderer_init(opaque);
    g_autofree Expected *expected = g_new(Expected, 1);

    renderer_fixture(s);
    renderer_texture_bind(s, mode == 1, environment);
    if (mode == 3) {
        kona_command(s, 0xc0, (uint32_t[]) { 0x1d01 }, 1);
    } else if (mode == 7) {
        stencil_config(s, 8);
        stencil_function(s, 0x207, 0, 255);
        stencil_operations(s, 0x1e02, 0x1e02, 0x1e02);
        kona_command(s, 0x24, (uint32_t[]) { 0xb71 }, 1);
        kona_command(s, 0xce, (uint32_t[]) { 0x207 }, 1);
        kona_command(s, 0xca, (uint32_t[]) { 0x206, fbits(0.25f) }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbc0 }, 1);
    } else if (mode == 8) {
        kona_command(s, 0xcb, (uint32_t[]) { 0x302, 0x303 }, 2);
        kona_command(s, 0x24, (uint32_t[]) { 0xbe2 }, 1);
    } else if (mode == 9) {
        kona_command(s, 0x21, (uint32_t[]) { 1, 0, 1, 0 }, 4);
    } else if (mode == 10) {
        kona_command(s, 0xc, (uint32_t[]) { 32, 16, 64, 80 }, 4);
        kona_command(s, 0x24, (uint32_t[]) { 0xc11 }, 1);
    } else if (mode == 12) {
        kona_command(s, 0xb4,
                     (uint32_t[]) { 0x3005, 0x3ff00000, 0, 0, 0, 0, 0,
                                    0xc0500000, 0 }, 9);
        kona_command(s, 0x24, (uint32_t[]) { 0x3005 }, 1);
    }
    gl_begin(s, 7);
    for (unsigned v = 0; v < 4; v++) {
        float w = mode == 2 ? 1 << v : 1;
        float x = positions[v][0], y = positions[v][1];

        if (mode == 11) {
            x = x ? 192 : -64;
            y = y ? 192 : -64;
        }
        kona_command_f(s, 0x6b, (float[]) {
            mode == 3 ? 0.25f + positions[v][0] / 256 : 0.5f,
            mode == 3 ? 0.5f : 0.25f,
            mode == 3 ? 0.75f - positions[v][1] / 256 : 0.75f, 0.5f,
        }, 4);
        kona_command_f(s, 0x88,
                       (float[]) { x / 128 * w, y / 128 * w, 0, w }, 4);
        kona_command_f(s, 0x94, (float[]) { x * w, y * w, 0, w }, 4);
    }
    gl_end(s);
    g_assert_cmpuint(renderer_counter(s, "x-test-gpu-requests"), >, 0);
    renderer_assert_draws(s, opaque, 1, 0, mode == 5);
    if (mode == 7) {
        g_assert_cmphex(stencil_colour_or_depth(s, 6), ==, 0x5b);
        g_assert_cmphex(stencil_colour_or_depth(s, 5), ==, 0x800000);
    }
    for (unsigned palette = 0; palette < 2; palette++) {
        expected_clear(expected, palette ? 0x326496 : 0xffffff);
        for (unsigned y = 0; y < 128; y++) {
            for (unsigned x = 0; x < 128; x++) {
                double u = (x + 0.5) / 128, v = (y + 0.5) / 128;
                double texel[4], colour[4] = { 0.5, 0.25, 0.75, 0.5 };
                const double env[4] = { 0.25, 0.5, 0.75, 0.25 };
                double alpha;
                uint8_t *pixel = expected->rgb + ((127 - y) * 128 + x) * 3;

                if ((mode == 10 &&
                     (x < 32 || x >= 96 || y < 16 || y >= 96)) ||
                    (mode == 12 && x < 64)) {
                    continue;
                }
                if (mode == 3) {
                    colour[0] = 0.25 + u * 0.5;
                    colour[1] = 0.5;
                    colour[2] = 0.75 - v * 0.5;
                }
                renderer_texture_sample(u, v, mode == 1, texel);
                for (unsigned c = 0; c < 4; c++) {
                    if (environment == 0) {
                        colour[c] = texel[c];
                    } else if (environment == 1) {
                        colour[c] *= texel[c];
                    } else if (environment == 2 && c < 3) {
                        colour[c] = colour[c] * (1 - texel[3]) +
                                    texel[c] * texel[3];
                    } else if (environment == 3) {
                        colour[c] = c == 3 ? colour[c] * texel[c] :
                            colour[c] * (1 - texel[c]) + env[c] * texel[c];
                    }
                }
                alpha = floor(colour[3] * 255 + 0.5);
                if (mode == 7 && alpha < 64) {
                    continue;
                }
                for (unsigned c = 0; c < 3; c++) {
                    double value = floor(colour[c] * 255 + 0.5);

                    if (mode == 8) {
                        value = floor(value * alpha / 255 + 255 - alpha + 0.5);
                    } else if (mode == 9 && c == 1) {
                        value = 255;
                    }
                    pixel[c] = value;
                }
            }
        }
        assert_window_matches(s, expected);
        kona_command(s, 1, (uint32_t[]) { 3, 0x22, 200, 400, 600 }, 5);
    }
    qtest_quit(s);
}

/*
 * A textured quad with a negative q at one vertex. The legacy host route
 * refuses it before sampling, and the scalar path drops the primitive
 * whose clipped coordinates are invalid, so the draw reports that once
 * and never reaches a per-fragment projected-coordinate report on either
 * path.
 */
static void test_renderer_texture_invalid_q_once(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *gpu = kona_init_logged_args(&log,
                                           " -global sgi-kona.host-gpu=on");
    QTestState *cpu = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=off");
    QTestState *states[] = { gpu, cpu };
    static const float positions[4][2] = {
        { 0, 0 }, { 128, 0 }, { 128, 128 }, { 0, 128 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(states); i++) {
        QTestState *s = states[i];

        renderer_fixture(s);
        renderer_texture_bind(s, false, 0);
        gl_begin(s, 7);
        for (unsigned v = 0; v < 4; v++) {
            float x = positions[v][0], y = positions[v][1];

            kona_command_f(s, 0x6b,
                           (float[]) { 0.5f, 0.25f, 0.75f, 0.5f }, 4);
            kona_command_f(s, 0x88, (float[]) { x / 128, y / 128, 0,
                                                v == 2 ? -0.5f : 0.5f }, 4);
            kona_command_f(s, 0x94, (float[]) { x, y, 0, 1 }, 4);
        }
        gl_end(s);
    }
    renderer_general_compare_planes(gpu, cpu, 0);
    qtest_quit(cpu);
    qtest_quit(gpu);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found,
                                       "invalid clipped coordinates"), ==, 1);
    g_assert_cmpuint(count_occurrences(found,
                                       "invalid projected coordinates"), ==, 0);
}

static void test_renderer_excluded_state(void)
{
    static const uint32_t enables[] = {
        0xb60,
        0xde0, 0x806f, 0xbd0, 0xb41, 0xb42,
        0xb24, 0xb20, 0xb10, 0xb50,
    };
    static const uint32_t visual_cases[][10] = {
        { UINT32_MAX, 12 }, { 0, 16 }, { 0, 12, 1 }, { 0, 12, 0, 2 },
        { 0, 12, 0, 0, 1 }, { 0, 12, 0, 0, 0, 1 },
        { 0, 12, 0, 0, 0, 0, 33 }, { 0, 12, 0, 0, 0, 0, 24, 1 },
        { 0, 12, 0, 0, 0, 0, 24, 0, 1 },
        { 0, 12, 0, 0, 0, 0, 24, 0, 0, 1 },
    };
    const char *node = g_getenv("QEMU_TEST_SGI_RENDER_NODE");
    QTestState *s;
    uint64_t fallbacks = 0;

    if (!node) {
        qtest_prereq_missing("SGI_RENDER_NODE",
                             "QEMU_TEST_SGI_RENDER_NODE is not set");
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
        qtest_prereq_missing("SGI_RENDER_NODE",
                             "QEMU_TEST_SGI_RENDER_NODE is not set");
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

static void assert_colour_matrix_isolated(QTestState *s)
{
    uint32_t expected[16] = { F1, 0, 0, 0, 0, F1, 0, 0,
                              0, 0, F1, 0, 0, 0, 0, F1 };

    expected[12] = fbits(0.25);
    assert_native_depth(s, 0xba0, (uint32_t[]){ 0x1800 }, 1);
    kona_command(s, 0xd2, (uint32_t[]){ 0 }, 1);
    kona_command_f(s, 0xd3, (float[16]) { 0 }, 16);
    kona_command_f(s, 0xd5, (float[16]) { 0 }, 16);
    kona_command_f(s, 0xdd, (float[]){ 2, 3, 4 }, 3);
    kona_command_f(s, 0xdb, (float[]){ 45, 0, 0, 1 }, 4);
    kona_command_f(s, 0xdf, (float[]){ 1, 2, 3 }, 3);
    kona_command(s, 0xd9, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0xd8, (uint32_t[]){ 0 }, 1);
    assert_native_depth(s, 0xba6, expected, 16);
    assert_native_depth(s, 0xba3, (uint32_t[]){ 1 }, 1);
    kona_command(s, 0xd0, (uint32_t[]){ 0x1700 }, 1);
    assert_native_depth(s, 0xba0, (uint32_t[]){ 0x1700 }, 1);
    assert_native_depth(s, 0xba6, expected, 16);
}

static void test_native_colour_matrix(gconstpointer opaque)
{
    bool migrate = GPOINTER_TO_INT(opaque);
    g_autofree char *tmpfs = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL;

    gl_make_current(from);
    kona_command_f(from, 0xdf, (float[]){ 0.25, 0, 0 }, 3);
    kona_command(from, 0xd0, (uint32_t[]){ 0x1800 }, 1);
    if (migrate) {
        tmpfs = g_dir_make_tmp("sn0-kona-colour-matrix-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
        args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
        to = qtest_init(args);
        sgi_migrate_stopped(from, uri);
    }
    assert_colour_matrix_isolated(to ? to : from);
    if (to) {
        qtest_quit(to);
        g_rmdir(tmpfs);
    }
    qtest_quit(from);
}

static void assert_native_error(QTestState *s, uint32_t error)
{
    prepare_refused_reply(s);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    assert_reply_posted(s);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, error);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 12), ==, 0xa5a5a5a5);
}

static void test_draw_pixels_begin_with_skips(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    gl_make_current(s);
    kona_command(s, 0x33, (uint32_t[]) { 0xcf3, 1 }, 2);
    gl_begin(s, 4);
    kona_command(s, 0x3a, (uint32_t[]) { 2, 2, 5, 1, 0, 0, 0, 0 }, 8);
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, REG_COMMAND, 0);
    }
    gl_end(s);
    assert_native_error(s, 0x502);
    assert_native_error(s, 0);
    qtest_quit(s);
}

static void test_native_error_state(void)
{
    static const uint32_t codes[] = {
        0x500, 0x501, 0x502, 0x503, 0x504, 0x505, 0x8031,
    };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    assert_native_error(s, 0);
    for (unsigned k = 0; k < ARRAY_SIZE(codes); k++) {
        kona_command(s, 0xe4, &codes[k], 1);
        kona_command(s, 0xe4, (uint32_t[]){ 0 }, 1);
        kona_command(s, 0xe4, (uint32_t[]){ 0x501 }, 1);
        kona_command(s, 0xe4, (uint32_t[]){ 0x506 }, 1);
        kona_command(s, 0xe4, (uint32_t[]){ UINT32_MAX }, 1);
        assert_native_error(s, codes[k]);
        assert_native_error(s, 0);
    }
    kona_command(s, 0xc9, (uint32_t[]){ 0x2000 }, 1);
    kona_command(s, 0xe4, (uint32_t[]){ 0x502 }, 1);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_error(s, 0x502);
    kona_command(s, 0xe4, (uint32_t[]){ 0x504 }, 1);
    kona_command(s, 0xc9, (uint32_t[]){ 0x2000 }, 1);
    assert_native_error(s, 0x504);
    kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
    assert_native_error(s, 0);
    kona_command(s, 0xe4, (uint32_t[]){ 0x503 }, 1);
    kona_command(s, 0x57, (uint32_t[]){ 0 }, 1);
    prepare_refused_reply(s);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    assert_reply_unchanged(s);
    kona_command(s, 0x71, (uint32_t[]){ 0 }, 1);
    assert_native_error(s, 0x503);
    kona_command(s, 0xe4, (uint32_t[]){ 0x503 }, 1);
    qtest_system_reset(s);
    gl_make_current(s);
    assert_native_error(s, 0);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "unrecognized native GL error 0x506"));
    g_assert_false(log_shows_misframing(found));
}

static void test_native_error_failed_reply(void)
{
    const uint64_t save_b = SAVE_AREA + 0x20000;
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    gl_make_current(s);
    kona_command(s, 0xe4, (uint32_t[]){ 0x500 }, 1);
    return_context_deactivate(s);
    prepare_refused_reply(s);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    assert_reply_unchanged(s);
    attrib_context_fresh(s, save_b);
    assert_native_error(s, 0);
    kona_command(s, 0xe4, (uint32_t[]){ 0x504 }, 1);
    return_context_deactivate(s);
    return_context_restore(s, SAVE_AREA);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(0x08000000),
                                      tagged_offset_word(0x08000000),
                                      0x4000 }, 3);
    prepare_refused_reply(s);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    assert_reply_unchanged(s);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    assert_native_error(s, 0x500);
    assert_native_error(s, 0);
    return_context_deactivate(s);
    return_context_restore(s, save_b);
    assert_native_error(s, 0x504);
    assert_native_error(s, 0);
    return_context_deactivate(s);
    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ physical_page_word(save_b + 0x20000),
                                      tagged_offset_word(save_b + 0x20000),
                                      0x10000 }, 3);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(0x08000000),
                                      tagged_offset_word(0x08000000),
                                      0x4000 }, 3);
    kona_command(s, 0xe4, (uint32_t[]){ 0x505 }, 1);
    for (unsigned k = 0; k < 2; k++) {
        kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found,
        "GL error reply failed; error 0x505 retained"), ==, 2);
    g_assert_nonnull(strstr(found, "4-byte write to"));
    g_assert_false(log_shows_misframing(found));
}

static void test_native_error_migrates(gconstpointer opaque)
{
    bool partial = GPOINTER_TO_INT(opaque);
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-kona-error-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    from = qtest_init(MACHINE_ARGS);
    to = qtest_init(args);
    gl_make_current(from);
    if (partial) {
        qtest_writel(from, REG_COMMAND, 0xe4);
    } else {
        kona_command(from, 0xe4, (uint32_t[]){ 0x8031 }, 1);
    }
    sgi_migrate_stopped(from, uri);
    if (partial) {
        qtest_writel(to, REG_COMMAND, 0x8031);
    }
    assert_native_error(to, 0x8031);
    assert_native_error(to, 0);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_texture_native_interior(gconstpointer opaque)
{
    bool migrate = GPOINTER_TO_INT(opaque);
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL;
    uint32_t h[20];

    gl_make_current(from);
    texture_native_bind(from, 0);
    memcpy(h, texture_header, sizeof(h));
    for (unsigned geometry = 0; geometry < 2; geometry++) {
        h[16] = geometry ? 0x11 : 0x23;
        kona_command(from, 0x174, h, 20);
        for (unsigned k = 0; k < 4; k++) {
            qtest_writel(from, REG_COMMAND,
                         geometry ? 0x0000ffff : 0xff0000ff);
        }
        kona_command(from, 0x176, (uint32_t[]) { 0 }, 1);
    }
    if (migrate) {
        dir = g_dir_make_tmp("sn0-kona-interior-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
        to = qtest_init(args);
        sgi_migrate_stopped(from, uri);
    }
    assert_texture_pixel(to ? to : from, 0, 0, 0xff0000ff);
    assert_texture_pixel(to ? to : from, 1, 1, 0xff0000ff);
    if (to) {
        qtest_quit(to);
        g_rmdir(dir);
    }
    qtest_quit(from);
}

static void test_texture_native_1d(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    bool dma = mode == 1 || mode == 3, half = mode == 2 || mode == 3;
    uint32_t h[20], bind[5] = {
        0, (1u << 28) | (41 << 12), (0x30 << 14) | (1 << 2), 1, 1,
    };
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10, 0x23 };
    uint32_t pixels[] = { 0xff0000ff, 0x0000ffff };
    uint32_t halves[] = { 0xffff0000, 0x0000ffff, 0, 0xffffffff };
    const uint32_t *data = half ? halves : pixels;
    unsigned count = half ? 4 : 2;
    static const double tri[3][3] = {
        { -0.8, -0.8, 0 }, { 0.8, -0.8, 0 }, { 0, 0.8, 0 },
    };
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL, *s = from;

    gl_make_current(s);
    gl_window_cleared(s, 0, 0, 0);
    kona_command(s, 0x106, bind, 5);
    kona_command(s, 0x179, record, 22);
    memcpy(h, texture_header, sizeof(h));
    h[0] = 0xde0;
    h[2] = 1;
    h[4] = half ? 3 : 1;
    h[6] = dma;
    h[7] = 0;
    h[14] = 0;
    h[17] = 0x2181 | (dma ? 4 : 0);
    kona_command(s, 0x174, h, 20);
    if (dma) {
        uint8_t bytes[16];

        for (unsigned k = 0; k < count; k++) {
            stl_be_p(bytes + 4 * k, data[k]);
        }
        qtest_memwrite(s, 0x380000, bytes, count * 4);
        qtest_writel(s, KONA_REG(0x100000), 0x20000380);
        texture_dma_packet(s, 0, count * 4, count * 4, 0);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x100);
    } else {
        for (unsigned k = 0; k < count; k++) {
            qtest_writel(s, REG_COMMAND, data[k]);
            if (mode == 10 && k == 0) {
                dir = g_dir_make_tmp("sn0-kona-1d-partial-XXXXXX", &err);
                g_assert_no_error(err);
                uri = g_strdup_printf("unix:%s/migrate.sock", dir);
                args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
                to = qtest_init(args);
                sgi_migrate_stopped(from, uri);
                s = to;
            }
        }
    }
    kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
    kona_command(s, 0x24, (uint32_t[]) { 0xde0 }, 1);
    if (mode == 5) {
        dir = g_dir_make_tmp("sn0-kona-1d-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
        to = qtest_init(args);
        sgi_migrate_stopped(from, uri);
        s = to;
    }
    if (mode == 8) {
        static const unsigned targets[] = { 0, 2, 4 };

        for (unsigned k = 0; k < ARRAY_SIZE(targets); k++) {
            bind[1] = (targets[k] << 28) | (41 << 12);
            kona_command(s, 0x106, bind, 5);
            kona_command(s, 0x179, record, 22);
            assert_texture_pixel(s, 0, 0, 0);
        }
        bind[1] = (1u << 28) | (41 << 12);
        bind[3] = 0x11;
        kona_command(s, 0x106, bind, 5);
        kona_command(s, 0x179, record, 22);
        assert_texture_pixel(s, 0, 0, 0);
        bind[3] = 1;
        kona_command(s, 0x106, bind, 5);
        kona_command(s, 0x179, record, 22);
        assert_texture_pixel(s, 0, 0, 0xff0000ff);
    }
    if (mode == 9) {
        for (unsigned bad = 0; bad < 3; bad++) {
            uint32_t invalid[20];

            memcpy(invalid, h, sizeof(invalid));
            if (bad == 0) {
                invalid[2] = 2;
                invalid[14] = 1;
            } else if (bad == 1) {
                invalid[12] = invalid[14] = 1;
            } else {
                invalid[15] = 1;
            }
            kona_command(s, 0x174, invalid, 20);
            for (unsigned k = 0; k < 2 * invalid[2]; k++) {
                qtest_writel(s, REG_COMMAND, 0x0000ffff);
            }
            kona_command(s, 0x176, (uint32_t[]) { 0 }, 1);
            assert_texture_pixel(s, 0, 0, 0xff0000ff);
        }
    }
    if (mode == 6 || mode == 7) {
        bool enabled = mode == 6;

        kona_command(s, enabled ? 0x24 : 0x25, (uint32_t[]) { 0xde0 }, 1);
        kona_command(s, 0xc9, (uint32_t[]) { 0x2000 }, 1);
        assert_reply_posted(s);
        kona_command(s, enabled ? 0x25 : 0x24, (uint32_t[]) { 0xde0 }, 1);
        kona_command(s, 0xc8, (uint32_t[]) { 0 }, 1);
    }
    kona_command_f(s, 0x63, (float[]) { 1, 1, 1 }, 3);
    if (mode == 4) {
        /* T must not turn magnification into minification. */
        gl_begin(s, 4);
        for (unsigned k = 0; k < 3; k++) {
            kona_command_f(s, 0x80,
                           (float[]) { 0.5, k == 1 ? 128 : 0 }, 2);
            kona_command_f(s, 0x90,
                           (float[]) { tri[k][0], tri[k][1], 0 }, 3);
        }
        gl_end(s);
        assert_stencil_colour(s, 0x0000ff);
    } else {
        kona_command_f(s, 0x80, (float[]) { 0.25, 123 }, 2);
        gl_triangle_z(s, tri, 0);
        assert_stencil_colour(s, mode == 7 ? 0xffffff : 0xff0000);
        if (mode != 7) {
            kona_command_f(s, 0x80, (float[]) { 0.75, -123 }, 2);
            gl_triangle_z(s, tri, 0);
            assert_stencil_colour(s, 0x0000ff);
        }
    }
    if (to) {
        qtest_quit(to);
        g_rmdir(dir);
    }
    qtest_quit(from);
}

int main(int argc, char **argv)
{
    static const uint32_t bad_format[] = { 0, 0, 1, 1, 14, 1, 0 };
    static const uint32_t bad_type[] = { 0, 0, 1, 1, 4, 7, 0 };
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

    take_group_paths(&argc, argv);
    g_test_init(&argc, &argv, NULL);

    add_case("sn0/origin2000/kona-3d/texture-curve/transport",
             test_texture_curve_transport);
    add_data_case("sn0/origin2000/kona-3d/texture-curve/migrate-empty",
                  GUINT_TO_POINTER(0), test_texture_curve_migration);
    add_data_case("sn0/origin2000/kona-3d/texture-curve/migrate-partial",
                  GUINT_TO_POINTER(13), test_texture_curve_migration);
    add_data_case("sn0/origin2000/kona-3d/texture-curve/migrate-padding",
                  GUINT_TO_POINTER(32), test_texture_curve_migration);
    add_data_case("sn0/origin2000/kona-3d/texture-curve/migrate-complete",
                  GUINT_TO_POINTER(33), test_texture_curve_migration);
    add_case("sn0/origin2000/kona-3d/every-command-frames",
             test_every_command_frames);
    add_case("sn0/origin2000/kona-3d/packed-forms-frame",
             test_packed_forms_frame);
    add_case("sn0/origin2000/kona-3d/light-model-not-bitmap",
             test_light_model_not_bitmap);
    add_case("sn0/origin2000/kona-3d/migration-mid-data-run",
             test_migration_mid_data_run);
    add_case("sn0/origin2000/kona-3d/ortho-half-size",
             test_ortho_half_size);
    add_case("sn0/origin2000/kona-3d/rotated-triangle",
             test_rotated_triangle);
    add_case("sn0/origin2000/kona-3d/perspective-quad",
             test_perspective_quad);
    add_case("sn0/origin2000/kona-3d/lit-flat-triangle",
             test_lit_flat_triangle);
    add_case("sn0/origin2000/kona-3d/packed-colour-across-migration",
             test_packed_colour_across_migration);
    add_case("sn0/origin2000/kona-3d/depth-less-and-greater",
             test_depth_less_and_greater);
    add_case("sn0/origin2000/kona-3d/depth-without-buffer",
             test_depth_without_buffer);
    add_case("sn0/origin2000/kona-3d/blended-triangle",
             test_blended_triangle);
    add_case("sn0/origin2000/kona-3d/culled-triangle",
             test_culled_triangle);
    add_case("sn0/origin2000/kona-3d/draw-pixels-begin-with-skips",
             test_draw_pixels_begin_with_skips);
    add_case("sn0/origin2000/kona-3d/read-pixels", test_read_pixels);
    add_case("sn0/origin2000/kona-3d/read-capacity",
             test_read_pixels_capacity);
    add_case("sn0/origin2000/kona-3d/read-visual-records",
             test_read_visual_records);
    add_case("sn0/origin2000/kona-3d/read-ambiguous-level",
             test_read_ambiguous_level_preserves_source);
    add_case("sn0/origin2000/kona-3d/read-partial-records",
             test_read_partial_records);
    add_case("sn0/origin2000/kona-3d/read-reply-order",
             test_read_reply_order);
    add_case("sn0/origin2000/kona-3d/read-coordinates",
             test_read_coordinates);
    add_case("sn0/origin2000/kona-3d/read-indexed-source-refused",
             test_read_indexed_source_refused);
    add_case("sn0/origin2000/kona-3d/read-double-buffers",
             test_read_double_buffers);
    add_case("sn0/origin2000/kona-3d/read-selection",
             test_read_selection);
    add_case("sn0/origin2000/kona-3d/get-queries", test_get_queries);
    add_case("sn0/origin2000/kona-3d/native-depth-state",
             test_native_depth_state);
    add_case("sn0/origin2000/kona-3d/native-state-migrates",
             test_native_state_migrates);
    add_case("sn0/origin2000/kona-3d/push-and-pop-attrib",
             test_push_and_pop_attrib);
    add_case("sn0/origin2000/kona-3d/native-state/bytes",
             test_native_query_bytes);
    add_case("sn0/origin2000/kona-3d/native-state/indexed-bits",
             test_native_indexed_query_bits);
    add_case("sn0/origin2000/kona-3d/native-state/invalid",
             test_native_query_invalid);
    add_case("sn0/origin2000/kona-3d/native-state/attrib",
             test_native_query_attrib);
    add_case("sn0/origin2000/kona-3d/native-state/stencil-pixels",
             test_native_stencil_query_pixels);
    add_case("sn0/origin2000/kona-3d/native-state/contexts",
             test_native_query_contexts);
    add_case("sn0/origin2000/kona-3d/native-state/reset",
             test_native_query_reset);
    add_data_case("sn0/origin2000/kona-3d/native-state/migration",
                  GUINT_TO_POINTER(0), test_native_query_migration);
    add_data_case("sn0/origin2000/kona-3d/native-state/"
                  "function-migration",
                  GUINT_TO_POINTER(1), test_native_query_migration);
    add_data_case("sn0/origin2000/kona-3d/native-state/"
                  "operations-migration",
                  GUINT_TO_POINTER(2), test_native_query_migration);
    add_data_case("sn0/origin2000/kona-3d/native-state/enable-migration",
                  GUINT_TO_POINTER(3), test_native_query_migration);
    add_data_case("sn0/origin2000/kona-3d/native-state/default-migration",
                  GUINT_TO_POINTER(4), test_native_query_migration);
    add_data_case("sn0/origin2000/kona-3d/native-state/no-context",
                  GUINT_TO_POINTER(0), test_native_query_refused);
    add_data_case("sn0/origin2000/kona-3d/native-state/missing-page",
                  GUINT_TO_POINTER(1), test_native_query_refused);
    add_data_case("sn0/origin2000/kona-3d/native-state/failed-page",
                  GUINT_TO_POINTER(2), test_native_query_refused);
    add_case("sn0/origin2000/kona-3d/hints/invalid",
             test_native_hints_invalid);
    add_case("sn0/origin2000/kona-3d/hints/attrib",
             test_native_hints_attrib);
    add_case("sn0/origin2000/kona-3d/hints/contexts",
             test_native_hints_contexts);
    add_case("sn0/origin2000/kona-3d/hints/reset",
             test_native_hints_reset);
    add_data_case("sn0/origin2000/kona-3d/hints/migration",
                  GUINT_TO_POINTER(0), test_native_hints_migration);
    add_data_case("sn0/origin2000/kona-3d/hints/"
                  "partial-selector-migration",
                  GUINT_TO_POINTER(1), test_native_hints_migration);
    add_data_case("sn0/origin2000/kona-3d/hints/partial-target-migration",
                  GUINT_TO_POINTER(2), test_native_hints_migration);
    add_data_case("sn0/origin2000/kona-3d/hints/omitted-migration",
                  GUINT_TO_POINTER(3), test_native_hints_migration);
    add_data_case("sn0/origin2000/kona-3d/hints/no-context",
                  GUINT_TO_POINTER(0), test_native_hints_refused);
    add_data_case("sn0/origin2000/kona-3d/hints/missing-page",
                  GUINT_TO_POINTER(1), test_native_hints_refused);
    add_data_case("sn0/origin2000/kona-3d/hints/failed-page",
                  GUINT_TO_POINTER(2), test_native_hints_refused);
    add_case("sn0/origin2000/kona-3d/hints/basic",
             test_native_hints);
    add_case("sn0/origin2000/kona-3d/attrib-depth/basic",
             test_native_attrib_depth);
    add_case("sn0/origin2000/kona-3d/attrib-depth/limits",
             test_native_attrib_depth_limits);
    add_case("sn0/origin2000/kona-3d/attrib-depth/reset",
             test_native_attrib_depth_reset);
    add_case("sn0/origin2000/kona-3d/attrib-depth/contexts",
             test_native_attrib_depth_contexts);
    add_data_case("sn0/origin2000/kona-3d/attrib-depth/migrates",
                  GINT_TO_POINTER(false),
                  test_native_attrib_depth_migrates);
    add_data_case("sn0/origin2000/kona-3d/attrib-depth/empty-migrates",
                  GINT_TO_POINTER(true),
                  test_native_attrib_depth_migrates);
    add_data_case("sn0/origin2000/kona-3d/attrib-depth/no-context",
                  GUINT_TO_POINTER(0), test_native_attrib_depth_refused);
    add_data_case("sn0/origin2000/kona-3d/attrib-depth/missing-page",
                  GUINT_TO_POINTER(1), test_native_attrib_depth_refused);
    add_data_case("sn0/origin2000/kona-3d/attrib-depth/failed-page",
                  GUINT_TO_POINTER(2), test_native_attrib_depth_refused);
    add_case("sn0/origin2000/kona-3d/attrib-stack-migrates",
             test_attrib_stack_migrates);


    add_case("sn0/origin2000/kona-3d/texture/widths",
             test_texture_widths);
    add_case("sn0/origin2000/kona-3d/texture/reset",
             test_texture_reset);
    add_case("sn0/origin2000/kona-3d/texture/migrates",
             test_texture_migrates);
    add_case("sn0/origin2000/kona-3d/texture/refused",
             test_texture_refused);
    add_data_case("sn0/origin2000/kona-3d/texture/byte/alpha",
                  GUINT_TO_POINTER(0), test_texture_native_byte_formats);
    add_data_case("sn0/origin2000/kona-3d/texture/byte/rgb-to-luminance",
                  GUINT_TO_POINTER(1), test_texture_native_byte_formats);
    add_data_case("sn0/origin2000/kona-3d/texture/byte/luminance",
                  GUINT_TO_POINTER(2), test_texture_native_byte_formats);
    add_data_case("sn0/origin2000/kona-3d/texture/byte/luminance-alpha",
                  GUINT_TO_POINTER(3), test_texture_native_byte_formats);
    add_data_case("sn0/origin2000/kona-3d/texture/byte/migrate-rows",
                  GUINT_TO_POINTER(2), test_texture_byte_migrates);
    add_data_case("sn0/origin2000/kona-3d/texture/byte/migrate-complete",
                  GUINT_TO_POINTER(5), test_texture_byte_migrates);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/contiguous",
                  GUINT_TO_POINTER(0), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/strided",
                  GUINT_TO_POINTER(1), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/unmapped",
                  GUINT_TO_POINTER(2), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/short",
                  GUINT_TO_POINTER(3), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/extra",
                  GUINT_TO_POINTER(4), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/prefix-mismatch",
                  GUINT_TO_POINTER(5), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/chunk-boundary",
                  GUINT_TO_POINTER(6), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/migrate-pending",
                  GUINT_TO_POINTER(7), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/migrate-complete",
                  GUINT_TO_POINTER(8), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/reset",
                  GUINT_TO_POINTER(9), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/overlap-refused",
                  GUINT_TO_POINTER(10), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/page-boundary",
                  GUINT_TO_POINTER(11), test_texture_native_dma);
    static const char *texture_dma_skip_cases[] = {
        "contiguous-row-skips", "whole-pixel-skips", "strided-row-skips",
        "skips-pending-migrates", "captured-transfer", "skipped-phase-refused",
        "skipped-late-unmapped",
    };
    for (unsigned k = 0; k < ARRAY_SIZE(texture_dma_skip_cases); k++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/dma/%s", texture_dma_skip_cases[k]);

        add_data_case(path, GUINT_TO_POINTER(13 + k),
                      test_texture_native_dma);
    }
    static const char *texture_source_cases[] = {
        "red-pio", "green-pio", "blue-pio", "alpha-pio", "luminance-pio",
        "luminance-alpha-pio", "bgr-pio", "bgra-pio", "abgr-pio", "red-dma",
        "green-dma", "blue-dma", "alpha-dma", "luminance-dma",
        "luminance-alpha-dma", "bgr-dma", "bgra-dma", "abgr-dma",
        "partial-pio-migrates", "pending-dma-migrates",
        "partial-pio-context-aborted",
    };
    for (unsigned k = 0; k < ARRAY_SIZE(texture_source_cases); k++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/source-colour/%s",
            texture_source_cases[k]);

        add_data_case(path, GUINT_TO_POINTER(k),
                      test_texture_ordinary_source);
    }
    add_case("sn0/origin2000/kona-3d/texture/native-draw",
             test_texture_native_draw);
    add_case("sn0/origin2000/kona-3d/texture/native-rows",
             test_texture_native_rows);
    add_data_case("sn0/origin2000/kona-3d/texture/rgb/source-rgb",
                  GUINT_TO_POINTER(4), test_texture_native_rgb);
    add_data_case("sn0/origin2000/kona-3d/texture/rgb/source-rgba",
                  GUINT_TO_POINTER(5), test_texture_native_rgb);
    static const char *volume_cases[] = {
        "nearest", "linear", "dma", "migrate-partial", "missing-slice",
        "mipmap-r", "mipmap-migrates", "restore-enabled", "restore-disabled",
        "physical-mismatch",
    };

    for (unsigned mode = 0; mode < ARRAY_SIZE(volume_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/volume/%s", volume_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_texture_native_volume);
    }
    add_case("sn0/origin2000/kona-3d/texture/negative-bound-filters",
             test_texture_negative_bound_filters);
    static const char *detail_cases[] = {
        "add", "modulate", "colour", "alpha", "absent", "missing",
        "migrates", "invalid-retained", "scaled", "minimum-rho-quarter",
        "minimum-rho-half", "maximum-rho-quarter", "maximum-rho-half",
        "negative-bound-migrates",
    };

    for (unsigned mode = 0; mode < ARRAY_SIZE(detail_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/detail/%s", detail_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_texture_native_detail);
    }
    /*
     * Every integer type converts through PIO. The quick setup sends one
     * type each way the other transports carry it: four-byte signed by DMA
     * and swapped DMA, and two-byte signed through swapped PIO.
     */
    for (unsigned type = 0; type < 6; type++) {
        g_autofree char *pio = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/integer/pio-%u", type);
        g_autofree char *dma = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/integer/dma-%u", type);
        g_autofree char *swap_pio = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/integer/swap-pio-%u", type);
        g_autofree char *swap_dma = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/integer/swap-dma-%u", type);
        bool every = g_test_slow();

        add_data_case(pio, GUINT_TO_POINTER(type),
                      test_texture_native_integer);
        if (every || type == 4) {
            add_data_case(dma, GUINT_TO_POINTER(type | 8),
                          test_texture_native_integer);
        }
        if (every || type == 2) {
            add_data_case(swap_pio, GUINT_TO_POINTER(type | 32),
                          test_texture_native_integer);
        }
        if (every || type == 4) {
            add_data_case(swap_dma, GUINT_TO_POINTER(type | 32 | 8),
                          test_texture_native_integer);
        }
    }
    add_data_case("sn0/origin2000/kona-3d/texture/integer/"
                  "migrate-partial", GUINT_TO_POINTER(3 | 16),
                  test_texture_native_integer);
    add_data_case("sn0/origin2000/kona-3d/texture/integer/"
                  "swap-migrate-partial", GUINT_TO_POINTER(3 | 32 | 16),
                  test_texture_native_integer);
    add_data_case("sn0/origin2000/kona-3d/texture/integer/"
                  "swap-migrate-state", GUINT_TO_POINTER(3 | 32 | 64),
                  test_texture_native_integer);
    add_data_case("sn0/origin2000/kona-3d/texture/float/pio",
                  GUINT_TO_POINTER(0), test_texture_native_float);
    add_data_case("sn0/origin2000/kona-3d/texture/float/dma",
                  GUINT_TO_POINTER(1), test_texture_native_float);
    add_data_case("sn0/origin2000/kona-3d/texture/float/nonfinite",
                  GUINT_TO_POINTER(2), test_texture_native_float);
    add_data_case("sn0/origin2000/kona-3d/texture/float/swap-pio",
                  GUINT_TO_POINTER(3), test_texture_native_float);
    add_data_case("sn0/origin2000/kona-3d/texture/float/swap-dma",
                  GUINT_TO_POINTER(4), test_texture_native_float);
    add_data_case("sn0/origin2000/kona-3d/texture/float/swap-nonfinite",
                  GUINT_TO_POINTER(5), test_texture_native_float);
    add_data_case("sn0/origin2000/kona-3d/texture/float/"
                  "migration/partial-pio",
                  GUINT_TO_POINTER(0), test_texture_float_migration);
    add_data_case("sn0/origin2000/kona-3d/texture/float/"
                  "migration/complete-pio",
                  GUINT_TO_POINTER(1), test_texture_float_migration);
    add_data_case("sn0/origin2000/kona-3d/texture/float/"
                  "migration/pending-dma",
                  GUINT_TO_POINTER(2), test_texture_float_migration);
    add_data_case("sn0/origin2000/kona-3d/texture/float/"
                  "migration/complete-dma",
                  GUINT_TO_POINTER(3), test_texture_float_migration);
    add_data_case("sn0/origin2000/kona-3d/texture/dma/trace-source",
                  GUINT_TO_POINTER(12), test_texture_native_dma);
    add_data_case("sn0/origin2000/kona-3d/texture/native-migrate-rows",
                  GUINT_TO_POINTER(2), test_texture_native_migrates);
    add_data_case("sn0/origin2000/kona-3d/texture/"
                  "native-migrate-complete", GUINT_TO_POINTER(4),
                  test_texture_native_migrates);
    add_case("sn0/origin2000/kona-3d/texture/publication",
             test_texture_native_publication);
    add_data_case("sn0/origin2000/kona-3d/texture/rebind-a-b-a",
                  GUINT_TO_POINTER(0), test_texture_rebind_control);
    add_data_case("sn0/origin2000/kona-3d/texture/rebind-reverse-upload",
                  GUINT_TO_POINTER(1), test_texture_rebind_control);
    add_data_case("sn0/origin2000/kona-3d/texture/rebind-8x8-normal",
                  GUINT_TO_POINTER(2), test_texture_rebind_control);
    add_case("sn0/origin2000/kona-3d/texture/rebind-refusals",
             test_texture_rebind_refusals);
    add_case("sn0/origin2000/kona-3d/texture/rebind-missing-draw",
             test_texture_rebind_missing_draw);
    add_case("sn0/origin2000/kona-3d/texture/rebind-missing-migration",
             test_texture_rebind_missing_migration);
    add_case("sn0/origin2000/kona-3d/texture/rebind-recovery-migration",
             test_texture_rebind_recovery_migration);
    add_data_case(
  "sn0/origin2000/kona-3d/texture/rebind-refusal-context",
  GUINT_TO_POINTER(1), test_texture_rebind_refusal_lifetime);
    add_data_case(
  "sn0/origin2000/kona-3d/texture/rebind-refusal-invalid-bind",
  GUINT_TO_POINTER(0), test_texture_rebind_refusal_lifetime);
    add_data_case(
  "sn0/origin2000/kona-3d/texture/rebind-refusal-interrupted",
  GUINT_TO_POINTER(2), test_texture_rebind_refusal_lifetime);
    add_case(
  "sn0/origin2000/kona-3d/texture/rebind-standalone-recovery",
  test_texture_rebind_standalone_recovery);
    add_case("sn0/origin2000/kona-3d/texture/rebind-contexts",
             test_texture_rebind_contexts);
    add_case("sn0/origin2000/kona-3d/texture/rebind-reset",
             test_texture_rebind_reset);
    add_case("sn0/origin2000/kona-3d/texture/rebind-migration",
             test_texture_rebind_migration);
    add_case("sn0/origin2000/kona-3d/texture/"
             "rebind-migration-missing-object",
             test_texture_rebind_migration_rejects_missing_object);
    add_case("sn0/origin2000/kona-3d/texture/"
             "rebind-migration-absent-table",
             test_texture_rebind_migration_rejects_absent_table);
    add_case("sn0/origin2000/kona-3d/texture/rebind-pending-migration",
             test_texture_rebind_pending_migration);
    add_case("sn0/origin2000/kona-3d/texture/rebind-split-record",
             test_texture_rebind_split_record);
    add_case("sn0/origin2000/kona-3d/texture/rebind-split-bind",
             test_texture_rebind_split_bind);
    add_case("sn0/origin2000/kona-3d/texture/rebind-interrupted",
             test_texture_rebind_interrupted);
    add_case("sn0/origin2000/kona-3d/texture/captured",
             test_texture_captured);
    add_case("sn0/origin2000/kona-3d/unmodeled-queries-refused",
             test_unmodeled_queries_refused);
    add_case("sn0/origin2000/kona-3d/unknown-query-refused",
             test_unknown_query_refused);
    add_data_case("sn0/origin2000/kona-3d/read-format-refused",
                  bad_format, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-type-refused",
                  bad_type, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-size-refused",
                  too_large, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-huge-width-refused",
                  huge_width, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-raw-enums-refused",
                  raw_enums, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-low-bits-refused",
                  low_bits, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-zero-width-refused",
                  zero_width, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-zero-height-refused",
                  zero_height, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-negative-width-refused",
                  negative_width, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-ambiguous-extent-refused",
                  ambiguous, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-clipped-refused",
                  clipped, test_read_pixels_refused);
    add_data_case("sn0/origin2000/kona-3d/read-missing-page-refused",
                  GINT_TO_POINTER(false), test_read_destination_refused);
    add_data_case("sn0/origin2000/kona-3d/read-failed-page-refused",
                  GINT_TO_POINTER(true), test_read_destination_refused);
    add_data_case("sn0/origin2000/kona-3d/push-texture-refused",
                  GUINT_TO_POINTER(0x40000), test_push_attrib_refused);
    add_data_case("sn0/origin2000/kona-3d/push-transform-refused",
                  GUINT_TO_POINTER(0x1000), test_push_attrib_refused);
    add_case("sn0/origin2000/kona-3d/reply-without-context-refused",
             test_reply_without_context_refused);

    add_case("sn0/origin2000/kona-3d/current-colour-rgb",
             test_current_colour_rgb);
    add_case("sn0/origin2000/kona-3d/return-context-isolation",
             test_return_context_isolation);
    add_data_case("sn0/origin2000/kona-3d/invalid-draw-migrates",
                  GUINT_TO_POINTER(1), test_return_source_migrates);
    add_data_case("sn0/origin2000/kona-3d/invalid-read-migrates",
                  GUINT_TO_POINTER(4), test_return_source_migrates);
    add_data_case("sn0/origin2000/kona-3d/unknown-binding-migrates",
                  GUINT_TO_POINTER(0x114), test_return_source_migrates);
    add_data_case("sn0/origin2000/kona-3d/partial-draw-migrates",
                  GUINT_TO_POINTER(0x1001), test_return_source_migrates);
    add_data_case("sn0/origin2000/kona-3d/partial-read-migrates",
                  GUINT_TO_POINTER(0x1004), test_return_source_migrates);
    add_data_case("sn0/origin2000/kona-3d/partial-binding-migrates",
                  GUINT_TO_POINTER(0x1114), test_return_source_migrates);

    add_data_case("sn0/origin2000/kona-3d/read-destroyed-replaced",
                  GINT_TO_POINTER(false),
                  test_read_destroyed_window_replaced);
    add_data_case("sn0/origin2000/kona-3d/read-replaced-migrates",
                  GINT_TO_POINTER(true),
                  test_read_destroyed_window_replaced);

    add_data_case("sn0/origin2000/kona-3d/retired-invalid-draw",
                  GUINT_TO_POINTER(1), test_read_retired_state);
    add_data_case("sn0/origin2000/kona-3d/retired-invalid-read",
                  GUINT_TO_POINTER(4), test_read_retired_state);
    add_data_case("sn0/origin2000/kona-3d/retired-unknown-binding",
                  GUINT_TO_POINTER(0x114), test_read_retired_state);
    add_case("sn0/origin2000/kona-3d/placed-move-keeps-swap",
             test_read_placed_move_keeps_swap);

    add_data_case("sn0/origin2000/kona-3d/read-single-retired-replaced",
                  GINT_TO_POINTER(false),
                  test_read_single_retired_replaced);
    add_data_case("sn0/origin2000/kona-3d/read-single-retired-migrates",
                  GINT_TO_POINTER(true),
                  test_read_single_retired_replaced);

    add_case("sn0/origin2000/kona-3d/stencil/visual-present-and-absent",
             test_stencil_visual_present_and_absent);

    add_case("sn0/origin2000/kona-3d/stencil/comparison-order-and-mask",
             test_stencil_comparison_order_and_mask);
    add_case("sn0/origin2000/kona-3d/stencil/operation-range",
             test_stencil_operation_range);
    add_case("sn0/origin2000/kona-3d/stencil/outcome-operations",
             test_stencil_outcome_operations);
    add_case("sn0/origin2000/kona-3d/stencil/only-clear",
             test_stencil_only_clear);
    add_case("sn0/origin2000/kona-3d/stencil/alpha-and-disabled-tests",
             test_stencil_alpha_and_disabled_tests);
    add_case("sn0/origin2000/kona-3d/stencil/masks-and-colour-targets",
             test_stencil_masks_and_colour_targets);
    add_case("sn0/origin2000/kona-3d/stencil/"
             "incomplete-or-invalid-visual-refused",
             test_stencil_incomplete_or_invalid_visual_refused);
    add_case("sn0/origin2000/kona-3d/stencil/clear-scissor-and-clip",
             test_stencil_clear_scissor_and_clip);
    add_case("sn0/origin2000/kona-3d/stencil/context-and-attrib",
             test_stencil_context_and_attrib);
    add_case("sn0/origin2000/kona-3d/stencil/reset-and-partial-packets",
             test_stencil_reset_and_partial_packets);
    add_case("sn0/origin2000/kona-3d/stencil/state-and-plane-migrate",
             test_stencil_state_and_plane_migrate);
    add_case("sn0/origin2000/kona-3d/stencil/"
             "visual-readback-remains-refused",
             test_stencil_visual_readback_remains_refused);
    add_case("sn0/origin2000/kona-3d/stencil/completion/"
             "partial-visual-migration",
             test_stencil_partial_visual_migration);
    add_case("sn0/origin2000/kona-3d/stencil/completion/"
             "accumulation-clear-diagnostic",
             test_stencil_accumulation_clear_diagnostic);
    add_case("sn0/origin2000/kona-3d/stencil/completion/"
             "invalid-state-enumerants",
             test_stencil_invalid_state_enumerants);
    add_case("sn0/origin2000/kona-3d/stencil/completion/"
             "destroyed-context-pixels-retained",
             test_stencil_destroyed_context_pixels_retained);
    add_case("sn0/origin2000/kona-3d/depth-blend/"
             "depth-clear-preserves-colour",
             test_depth_clear_preserves_colour);
    add_case("sn0/origin2000/kona-3d/depth-blend/"
             "colour-masks-preserve-channels",
             test_colour_masks_preserve_channels);
    add_case("sn0/origin2000/kona-3d/depth-blend/"
             "depth-plane-migration",
             test_depth_plane_migration);
    add_case("sn0/origin2000/kona-3d/depth-blend/"
             "blend-without-alpha-plane",
             test_blend_without_alpha_plane);
    add_case("sn0/origin2000/kona-3d/texture-8x8/captured",
             test_texture_8x8_captured);
    add_case("sn0/origin2000/kona-3d/texture-8x8/native-prefix",
             test_texture_8x8_native_prefix);
    add_case("sn0/origin2000/kona-3d/texture-prefix/native-cleanup",
             test_texture_native_cleanup);
    add_data_case("sn0/origin2000/kona-3d/texture-prefix/widths",
                  GUINT_TO_POINTER(0x1c4), test_texture_prefix_widths);
    add_data_case("sn0/origin2000/kona-3d/texture-cleanup-prefix/widths",
                  GUINT_TO_POINTER(0x1c5), test_texture_prefix_widths);
    add_data_case("sn0/origin2000/kona-3d/texture-prefix/reset",
                  GUINT_TO_POINTER(0x1c4), test_texture_prefix_reset);
    add_data_case("sn0/origin2000/kona-3d/texture-cleanup-prefix/reset",
                  GUINT_TO_POINTER(0x1c5), test_texture_prefix_reset);
    add_data_case("sn0/origin2000/kona-3d/texture-prefix/migration",
                  GUINT_TO_POINTER(0x1c4), test_texture_prefix_migration);
    add_data_case("sn0/origin2000/kona-3d/texture-cleanup-prefix/"
                  "migration",
                  GUINT_TO_POINTER(0x1c5), test_texture_prefix_migration);
    add_data_case("sn0/origin2000/kona-3d/texture-prefix/handles",
                  GUINT_TO_POINTER(0x1c4),
                  test_texture_prefix_handles);
    add_data_case("sn0/origin2000/kona-3d/texture-cleanup-prefix/"
                  "handles",
                  GUINT_TO_POINTER(0x1c5),
                  test_texture_prefix_handles);
    add_case("sn0/origin2000/kona-3d/texture-8x8/widths",
             test_texture_8x8_widths);
    add_case("sn0/origin2000/kona-3d/texture-8x8/reset",
             test_texture_8x8_reset);
    add_case("sn0/origin2000/kona-3d/texture-8x8/migration",
             test_texture_8x8_migrates);
    add_data_case("sn0/origin2000/kona-3d/texture-8x8/refused",
                  GUINT_TO_POINTER(1), test_texture_8x8_refused);
    add_data_case(
  "sn0/origin2000/kona-3d/texture-8x8/refused-unknown-span",
                  GUINT_TO_POINTER(0), test_texture_8x8_refused);
    add_data_case("sn0/origin2000/kona-3d/renderer/no-render-node",
                  GUINT_TO_POINTER(0), test_renderer_fixture);
    add_data_case("sn0/origin2000/kona-3d/renderer/typed-coverage",
                  GUINT_TO_POINTER(0), test_renderer_typed_coverage);
    add_data_case("sn0/origin2000/kona-3d/renderer/typed-same-rgb",
                  GUINT_TO_POINTER(1), test_renderer_typed_coverage);
    add_case("sn0/origin2000/kona-3d/renderer/typed-ortho-excluded",
             test_renderer_typed_ortho_excluded);
    add_case("sn0/origin2000/kona-3d/renderer/typed-failure-reset",
             test_renderer_typed_failure_reset);
    add_case("sn0/origin2000/kona-3d/renderer/typed-partial-window",
             test_renderer_typed_partial_window);
    add_case("sn0/origin2000/kona-3d/renderer/typed-checked-failure",
             test_renderer_typed_checked_failure);
    add_case("sn0/origin2000/kona-3d/renderer/legacy-supported-failure",
             test_renderer_legacy_failure);
    add_data_case("sn0/origin2000/kona-3d/renderer/mesa-refused/fog",
                  GUINT_TO_POINTER(0), test_renderer_mesa_refused);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "mesa-refused/alpha-test",
                  GUINT_TO_POINTER(1), test_renderer_mesa_refused);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "mesa-refused/mixed-frame",
                  GUINT_TO_POINTER(2), test_renderer_mesa_refused);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "texture-incomplete/refused",
                  GUINT_TO_POINTER(0), test_renderer_texture_incomplete);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "texture-incomplete/accepted",
                  GUINT_TO_POINTER(1), test_renderer_texture_incomplete);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "legacy-raster-enable/line-stipple",
                  GUINT_TO_POINTER(0),
                  test_renderer_legacy_raster_enable);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "legacy-raster-enable/line-smooth",
                  GUINT_TO_POINTER(1),
                  test_renderer_legacy_raster_enable);
    add_case("sn0/origin2000/kona-3d/renderer/legacy-mismatch",
             test_renderer_legacy_mismatch);
    add_data_case("sn0/origin2000/kona-3d/renderer/hold-oom/host",
                  GUINT_TO_POINTER(1), test_renderer_hold_oom);
    add_data_case("sn0/origin2000/kona-3d/renderer/hold-oom/scalar",
                  GUINT_TO_POINTER(2), test_renderer_hold_oom);
    add_case("sn0/origin2000/kona-3d/renderer/legacy-full-window",
             test_renderer_legacy_full_window);
    add_case("sn0/origin2000/kona-3d/renderer/legacy-diagonals",
             test_renderer_legacy_diagonals);
    add_case("sn0/origin2000/kona-3d/renderer/texture-invalid-q-once",
             test_renderer_texture_invalid_q_once);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "create-legacy-unavailable",
                  GUINT_TO_POINTER(1), test_renderer_create_failure);
    add_data_case("sn0/origin2000/kona-3d/renderer/create-legacy-oom",
                  GUINT_TO_POINTER(2), test_renderer_create_failure);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "create-legacy-host-error",
                  GUINT_TO_POINTER(3), test_renderer_create_failure);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "create-typed-unavailable",
                  GUINT_TO_POINTER(5), test_renderer_create_failure);
    add_data_case("sn0/origin2000/kona-3d/renderer/create-typed-oom",
                  GUINT_TO_POINTER(6), test_renderer_create_failure);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "create-typed-host-error",
                  GUINT_TO_POINTER(7), test_renderer_create_failure);
    add_case("sn0/origin2000/kona-3d/renderer/"
             "create-arm-cleared-on-load",
             test_renderer_create_arm_cleared_on_load);
    add_data_case("sn0/origin2000/kona-3d/renderer/real-gl",
                  GUINT_TO_POINTER(1), test_renderer_fixture);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "gpu-to-cpu-migration",
                  GUINT_TO_POINTER(1), test_renderer_migration);
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "cpu-to-gpu-migration",
                  GUINT_TO_POINTER(0), test_renderer_migration);
    for (unsigned mode = 0; mode < 10; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/renderer/general-triangle/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_renderer_general_triangle);
    }
    add_data_case("sn0/origin2000/kona-3d/renderer/"
                  "general-triangle/no-gpu",
                  GUINT_TO_POINTER(RENDERER_GENERAL_NO_GPU),
                  test_renderer_general_triangle);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/polygon/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/polygon/%u",
                       4, test_renderer_polygon);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/large-polygon/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/large-polygon/%u",
                       6, test_renderer_large_polygon);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/scissor/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/scissor/%u",
                       8, test_renderer_scissor);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/viewport/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/viewport/%u",
                       8, test_renderer_viewport);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/tiles/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/tiles/%u",
                       9, test_renderer_tiles);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/culling/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/culling/%u",
                       8, test_renderer_culling);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/colour-mask/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/colour-mask/%u",
                       32, test_renderer_colour_mask);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/logic-op/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/logic-op/%u",
                       80, test_renderer_logic_op);
    add_data_case(
  "sn0/origin2000/kona-3d/renderer/smooth-gpu-precision",
  GUINT_TO_POINTER(13), test_renderer_smooth);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/alpha-test/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/alpha-test/%u",
                       64, test_renderer_alpha_test);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/blend/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/blend/%u",
                       54, test_renderer_blend);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/smooth/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/smooth/%u",
                       13, test_renderer_smooth);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/depth/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/depth/%u",
                       64, test_renderer_depth);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/stencil/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/stencil/%u",
                       54, test_renderer_stencil);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/depth-gradient/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/depth-gradient/%u",
                       12, test_renderer_depth_gradient);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/volume-clip/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/volume-clip/%u",
                       10, test_renderer_volume_clip);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/user-clip/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/user-clip/%u",
                       12, test_renderer_user_clip);
    renderer_add_modes("sn0/origin2000/kona-3d/renderer/texture/%u",
                       "sn0/origin2000/kona-3d/renderer/cpu/texture/%u",
                       13, test_renderer_texture);
    add_case("sn0/origin2000/kona-3d/renderer/excluded-state",
             test_renderer_excluded_state);
    add_case("sn0/origin2000/kona-3d/native-bitmap/stale-destination",
             test_bitmap_stale_destination);
    static const char *native_family_cases[] = {
        "luminance16", "depth-format-refused", "comparison-refused",
        "unknown-comparison-refused", "record-family-refused",
        "legacy-family-layout-refused",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(native_family_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture-family/%s",
            native_family_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_texture_native_family);
    }
    static const char *native_bitmap_cases[] = {
        "placement-move", "zoom-ignored", "normalized-bitorder",
        "partial-migrates", "invalid-raster", "zero-image-move",
        "zero-image-invalid-raster", "begin-refused", "no-context",
        "incomplete-texture", "malformed-count", "nonfinite-origin",
        "pending-raw-replaced", "raster-depth", "clipped",
        "zero-image-textured",
        "partial-context-aborted", "contexts", "oversize-drained",
        "oversize-drain-migrates", "word-padding-ignored",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(native_bitmap_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-bitmap/%s",
            native_bitmap_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_native_bitmap);
    }
    add_data_case("sn0/origin2000/kona-3d/image-transfer-refusal/scale",
                  GUINT_TO_POINTER(0), test_native_image_state_refusal);
    add_data_case(
  "sn0/origin2000/kona-3d/image-transfer-refusal/unknown-transfer",
                  GUINT_TO_POINTER(1), test_native_image_state_refusal);
    add_data_case(
  "sn0/origin2000/kona-3d/image-transfer-refusal/contradictory-source",
  GUINT_TO_POINTER(2), test_native_image_state_refusal);
    add_data_case(
  "sn0/origin2000/kona-3d/image-transfer-refusal/pending-source",
  GUINT_TO_POINTER(3), test_native_image_state_refusal);
    add_case("sn0/origin2000/kona-3d/raw-bitmap/large-record",
             test_raw_bitmap_large);
    add_case("sn0/origin2000/kona-3d/copy-pixels/indexed-source-refused",
             test_copy_pixels_indexed_source);
    static const char *copy_query_cases[] = {
        "positive-zoom", "negative-zoom", "overlap-snapshot", "transfer",
        "clipped-source", "nonfinite-zoom", "nonfinite-transfer", "map-color",
        "incomplete-texture", "convolution-refused", "no-context",
        "unregistered-page",
        "unknown-read-binding", "unavailable-back", "huge-width", "negative-x",
        "empty-width", "packet-migrates",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(copy_query_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/copy-query/%s", copy_query_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_copy_pixels_query);
    }
    add_case("sn0/origin2000/kona-3d/copy-query/context-pages",
             test_copy_query_context_pages);
    static const char *copy_pixels_cases[] = {
        "color", "overlap", "transfer", "queried-zero-flags",
        "unknown-flags-refused", "split-refused", "depth-refused",
        "nonidentity-zoom-refused", "begin-refused", "default-color-map",
        "clipped-source-refused", "invalid-raster", "incomplete-texture",
        "no-context", "nonfinite-transfer-refused", "packet-migrates",
        "raster-position-preserved",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(copy_pixels_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/copy-pixels/%s", copy_pixels_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_copy_pixels);
    }
    static const char *bitmap_dma_cases[] = {
        "msb", "lsb", "skip-rows", "pending-migrates", "short-descriptor",
        "gap-refused", "unaligned-address", "late-unmapped-page",
        "context-changed", "unmapped-source", "invalid-placement",
        "strided-descriptor", "pending-narrow-replaced",
        "pending-strided-replaced", "pending-phase-replaced",
        "pending-skips-replaced", "contiguous-prefix", "contiguous-padding",
        "strided-phase", "strided-bit-skips", "strided-lsb",
        "strided-pending-migrates", "strided-whole-byte-skip",
        "strided-phase-refused", "strided-descriptor-refused",
        "strided-late-unmapped",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(bitmap_dma_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/bitmap-dma/%s", bitmap_dma_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_raw_bitmap_dma);
    }
    static const char *raw_bitmap_cases[] = {
        "strided", "contiguous", "skips", "lsb-first", "skips-migrates",
        "partial-migrates", "begin-refused", "incomplete-texture",
        "context-aborted", "slot-aborted", "transfer-zoom-ignored",
        "invalid-raster", "oversize-drained", "oversize-drain-migrates",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(raw_bitmap_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/raw-bitmap/%s", raw_bitmap_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_raw_bitmap);
    }
    static const char *transfer_cases[] = {
        "pio", "dma", "partial-migrates", "pixel-mode-pop", "combined-pop",
        "attrib-migrates", "contexts", "fresh-context-default", "reset-default",
        "nonfinite", "nonfinite-migrates", "begin-setter-refused",
        "unknown-sticky",
        "map-color-pop", "finite-restored", "irrelevant-state", "clamp",
        "before-alpha-test", "before-blend",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(transfer_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/draw-pixels-transfer/%s",
            transfer_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_draw_pixels_transfer);
    }
    static const char *draw_pixels_zoom_cases[] = {
        "pio", "dma", "negative-pio", "negative-dma", "downscale",
        "zero", "clipped", "migrates", "pixel-mode-pop", "combined-pop",
        "attrib-migrates", "contexts", "begin-setter-refused", "nonfinite",
        "finite-restored", "fresh-context-default", "reset-default",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(draw_pixels_zoom_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/draw-pixels-zoom/%s",
            draw_pixels_zoom_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_draw_pixels_zoom);
    }
    static const char *texture_layout_cases[] = {
        "rgb-contiguous-prefix", "rgb-contiguous-row-gap", "rgb-changing-phase",
        "rgb-unpack-skips", "rgb-contiguous-skip-rows", "skips-migrate",
        "contiguous-migrates", "destination-drains", "wrong-layout-refused",
        "float-contiguous-prefix", "float-contiguous-migrates",
        "float-strided-skips",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(texture_layout_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture-pio-layout/%s",
            texture_layout_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_texture_pio_layout);
    }
    static const char *texture_transfer_cases[] = {
        "pio", "dma", "partial-pio-migrates", "pending-dma-migrates",
        "complete-pio-migrates", "signed-before-bias", "float-before-transfer",
        "late-nan-atomic", "alpha-storage", "luminance-storage",
        "luminance-alpha-storage", "dma-captured-before-state-change",
        "ushort-swap",
        "unused-index-transfer-migrates",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(texture_transfer_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture-transfer/%s",
            texture_transfer_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_texture_pixel_transfer);
    }
    static const char *read_dma_cases[] = {
        "contiguous-phase0", "contiguous-phase1", "contiguous-phase2",
        "contiguous-phase3", "strided-phase0", "strided-phase1",
        "strided-phase2", "strided-phase3", "captured-pack-swap", "rgba-float",
        "captured-framebuffer", "captured-transfer", "repeat-finish",
        "staged-migrates", "descriptor-migrates", "partial-start-migrates",
        "partial-read-migrates", "partial-finish-migrates",
        "pending-context-cancelled", "staged-context-replaced", "double-read",
        "replacement-start", "missing-read", "missing-start", "wrong-reason",
        "bad-address-tag", "wrong-total", "wrong-line", "contiguous-gap",
        "nonzero-final-word", "oversize-span", "wrong-phase",
        "unmapped-destination", "late-unmapped-destination",
        "unsupported-format", "unsupported-type", "ambiguous-eight-by-one",
        "zero-width", "negative-height", "clipped-source",
        "nonfinite-transfer", "unmapped-reason-page", "swapped-stage-migrates",
        "partial-read-context-aborted", "partial-finish", "missing-reason-page",
        "no-context", "unmapped-page-offset",
        "inactive-load-clears-staged-destination",
        "staged-partial-read-aborted", "x-read-refused",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(read_dma_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/read-dma/%s", read_dma_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_read_pixels_dma);
    }
    static const char *colour_formats[] = {
        "alpha", "rgb", "rgba", "luminance", "luminance-alpha",
    };
    static const char *colour_types[] = {
        "byte", "ubyte", "short", "ushort", "int", "uint", "float",
    };
    static const char *read_dma_formats[] = {
        "red", "green", "blue", "alpha", "rgb", "rgba", "luminance",
        "luminance-alpha", "bgr", "bgra",
    };
    for (unsigned mode = 0; mode < 70; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/read-dma-typed/%s-%s",
            read_dma_formats[mode / 7], colour_types[mode % 7]);

        add_data_case(path, GUINT_TO_POINTER(64 + mode),
                      test_read_pixels_dma);
    }
    for (unsigned type = 0; type < 7; type++) {
        g_autofree char *read = g_strdup_printf(
            "sn0/origin2000/kona-3d/abgr-read/%s", colour_types[type]);
        g_autofree char *pack = g_strdup_printf(
            "sn0/origin2000/kona-3d/abgr-pack/%s", colour_types[type]);
        g_autofree char *dma = g_strdup_printf(
            "sn0/origin2000/kona-3d/abgr-dma/%s", colour_types[type]);

        add_data_case(read, GUINT_TO_POINTER(110 + type),
                      test_read_pixels_colour);
        add_data_case(pack, GUINT_TO_POINTER(68 + type),
                      test_read_pixels_pack_swap);
        add_data_case(dma, GUINT_TO_POINTER(134 + type),
                      test_read_pixels_dma);
    }
    static const char *depth_pixel_names[] = {
        "read-clear", "byte", "ubyte", "short", "ushort", "int", "uint",
        "float", "transfer", "attributes", "migration", "dma",
        "read-transfer", "read-dma", "copy-overlap", "read-buffer-independent",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(depth_pixel_names); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/depth-pixels/%s", depth_pixel_names[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_depth_pixels);
    }
    for (unsigned mode = 0; mode < 19; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/depth-boundaries/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_depth_pixel_boundaries);
    }
    for (unsigned mode = 0; mode < 48; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/colour-map-source-types/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_colour_map_source_types);
    }
    static const char *colour_map_names[] = {
        "draw", "read", "copy-overlap", "texture-pio", "texture-dma",
        "texture-partial-migration", "texture-dma-migration", "attributes",
        "partial-map-migration", "default", "ushort", "uint", "arithmetic",
        "clamping", "negative-zero", "late-nan", "contexts", "reset",
        "alpha-test", "draw-dma", "pending-texture-with-partial-map",
        "complete-texture-with-partial-map",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(colour_map_names); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/colour-maps/%s", colour_map_names[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_colour_pixel_maps);
    }
    for (unsigned mode = 0; mode < 37; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/indexed-colour-pixels/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_indexed_colour_pixels);
    }
    for (unsigned mode = 0; mode < 46; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/bitmap-index-pixels/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_bitmap_index_pixels);
    }
    for (unsigned mode = 0; mode < 7; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/bitmap-texture-dimensions/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_bitmap_texture_dimensions);
    }
    for (unsigned mode = 0; mode < 45; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/bitmap-texture-pixels/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_bitmap_texture_pixels);
    }
    for (unsigned mode = 0; mode < 42; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/indexed-texture-pixels/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_indexed_texture_pixels);
    }
    for (unsigned mode = 0; mode < 36; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/bitmap-stencil-read/%u", mode);

        if (mode >= 20 && mode < 24) {
            continue;
        }
        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_bitmap_stencil_read);
    }
    for (unsigned mode = 0; mode < 22; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-packed-index/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_native_packed_index);
    }
    for (unsigned mode = 0; mode < 16; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-index-setters/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_native_index_setters);
    }
    for (unsigned mode = 0; mode < 32; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-index-logic/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_native_index_logic);
    }
    for (unsigned mode = 0; mode < 4; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-index-state/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_native_index_state);
    }
    for (unsigned mode = 0; mode < 34; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-index-bitmap/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_native_index_bitmap);
    }
    for (unsigned mode = 0; mode < 56; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-index-sources/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_native_index_sources);
    }
    for (unsigned type = 0; type < 7; type++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-index-read-types/%u", type);

        add_data_case(path, GUINT_TO_POINTER(type),
                     test_native_index_read_types);
    }
    for (unsigned mode = 0; mode < 25; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/native-index-pixels/%u", mode);

        if (mode == 16 || mode == 17) {
            continue;
        }
        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_native_index_pixels);
    }
    static const char *pixel_map_size_names[] = {
        "default", "published", "replaced", "rejected", "attributes",
        "contexts", "reset", "migration", "partial-migration",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(pixel_map_size_names); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/pixel-map-sizes/%s",
            pixel_map_size_names[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_pixel_map_sizes);
    }
    static const char *stencil_map_names[] = {
        "float", "uint", "ushort", "default", "read", "copy", "attributes",
        "partial-migration", "maximum", "non-power-of-two", "unknown-map",
        "zero-size", "late-nan", "oversize", "unknown-capacity", "begin",
        "negative-float", "index-transfer",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(stencil_map_names); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/stencil-maps/%s", stencil_map_names[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_stencil_pixel_maps);
    }
    static const char *stencil_pixel_names[] = {
        "draw-read", "bypass-fragment-tests", "copy-overlap",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(stencil_pixel_names); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/stencil-pixels/%s",
            stencil_pixel_names[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_stencil_pixels);
    }
    for (unsigned mode = 0; mode < 61; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/stencil-transport/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_stencil_pixels_transport);
    }
    for (unsigned mode = 0; mode < 10; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/stencil-boundaries/%u", mode);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_stencil_pixels_boundaries);
    }
    for (unsigned type = 0; type <= 6; type++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/stencil-read-dma/%u", type);

        add_data_case(path, GUINT_TO_POINTER(type),
                     test_stencil_pixels_read_dma);
    }
    static const char *packed_names[] = {
        "rgb-332", "rgba-4444", "rgba-5551", "rgba-8888", "rgba-1010102",
        "bgra-4444", "bgra-5551", "bgra-8888", "bgra-1010102",
    };
    for (unsigned mode = 0; mode < 36; mode++) {
        g_autofree char *ingress = g_strdup_printf(
            "sn0/origin2000/kona-3d/packed-ingress/%s-%s%s",
            packed_names[mode % 9], (mode / 9) & 1 ? "dma" : "pio",
            (mode / 9) & 2 ? "-swapped" : "");
        g_autofree char *read = g_strdup_printf(
            "sn0/origin2000/kona-3d/packed-read/%s-%s%s",
            packed_names[mode % 9], (mode / 9) & 1 ? "dma" : "pio",
            (mode / 9) & 2 ? "-swapped" : "");

        add_data_case(ingress, GUINT_TO_POINTER(mode),
                      test_packed_pixel_ingress);
        add_data_case(read, GUINT_TO_POINTER(mode),
                      test_packed_pixel_read);
    }
    static const char *reordered_pixels_cases[] = {
        "bgr-pio", "bgra-pio", "bgr-ushort-swapped-dma", "bgra-float-dma",
        "abgr-pio", "abgr-ushort-swapped-dma", "abgr-float-dma",
    };
    for (unsigned k = 0; k < ARRAY_SIZE(reordered_pixels_cases); k++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/reordered-pixels/%s",
            reordered_pixels_cases[k]);

        add_data_case(path, GUINT_TO_POINTER(k), test_reordered_pixels);
    }
    for (unsigned mode = 0; mode < 14; mode++) {
        g_autofree char *read = g_strdup_printf(
            "sn0/origin2000/kona-3d/reordered-read/%s-%s",
            mode < 7 ? "bgr" : "bgra", colour_types[mode % 7]);
        g_autofree char *pack = g_strdup_printf(
            "sn0/origin2000/kona-3d/reordered-pack/%s-%s",
            mode < 7 ? "bgr" : "bgra", colour_types[mode % 7]);

        add_data_case(read, GUINT_TO_POINTER(96 + mode),
                      test_read_pixels_colour);
        add_data_case(pack, GUINT_TO_POINTER(54 + mode),
                      test_read_pixels_pack_swap);
    }
    static const char *single_formats[] = { "red", "green", "blue" };
    for (unsigned mode = 0; mode < 42; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/single-pixels/%s-%s-%s",
            mode < 21 ? "pio" : "dma", single_formats[(mode % 21) / 7],
            colour_types[mode % 7]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_single_channel_pixels);
    }
    static const char *single_pixels_cases[] = {
        "partial-pio-migrates", "pending-dma-migrates", "context-refused",
        "ushort-swapped", "transfer-default-channels",
    };
    for (unsigned k = 0; k < ARRAY_SIZE(single_pixels_cases); k++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/single-pixels/%s", single_pixels_cases[k]);

        add_data_case(path, GUINT_TO_POINTER(42 + k),
                      test_single_channel_pixels);
    }
    for (unsigned mode = 0; mode < 21; mode++) {
        g_autofree char *read = g_strdup_printf(
            "sn0/origin2000/kona-3d/single-read/%s-%s",
            single_formats[mode / 7], colour_types[mode % 7]);
        g_autofree char *pack = g_strdup_printf(
            "sn0/origin2000/kona-3d/single-pack/%s-%s",
            single_formats[mode / 7], colour_types[mode % 7]);

        add_data_case(read, GUINT_TO_POINTER(64 + mode),
                      test_read_pixels_colour);
        add_data_case(pack, GUINT_TO_POINTER(32 + mode),
                      test_read_pixels_pack_swap);
    }
    add_data_case("sn0/origin2000/kona-3d/single-pack/partial-migrates",
                  GUINT_TO_POINTER(53), test_read_pixels_pack_swap);
    for (unsigned mode = 0; mode < 35; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/colour-read/%s-%s",
            colour_formats[mode / 7], colour_types[mode % 7]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_read_pixels_colour);
    }
    add_data_case(
  "sn0/origin2000/kona-3d/colour-read/uint-rgb8-expansion",
                  GUINT_TO_POINTER(35), test_read_pixels_colour);
    add_data_case("sn0/origin2000/kona-3d/colour-read/int-rgb8-expansion",
                  GUINT_TO_POINTER(36), test_read_pixels_colour);
    static const char *luminance_cases[] = {
        "luminance-transfer-ubyte", "luminance-alpha-transfer-ushort",
        "luminance-transfer-float", "luminance-wide-cancellation",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(luminance_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/colour-read/%s", luminance_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_read_pixels_luminance_transfer);
    }
    static const char *typed_extent_cases[] = {
        "float-capacity", "float-capacity-exceeded", "ushort-ambiguous",
        "float-ambiguous",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(typed_extent_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/typed-read/%s", typed_extent_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_read_pixels_typed_extent);
    }
    static const char *pack_read_cases[] = {
        "rgb-byte", "rgb-ubyte", "rgb-short", "rgb-ushort", "rgb-int",
        "rgb-uint", "rgb-float", "rgba-byte", "rgba-ubyte", "rgba-short",
        "rgba-ushort", "rgba-int", "rgba-uint", "rgba-float", "la-byte",
        "la-ubyte", "la-short", "la-ushort", "la-int", "la-uint", "la-float",
        "default", "unpack-independent", "pack-with-unpack", "disabled",
        "partial-query-migrates", "contexts", "fresh-context", "reset",
        "server-attrib-independent", "alpha", "begin-refused",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(pack_read_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/pack-read/%s", pack_read_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_read_pixels_pack_swap);
    }
    static const char *typed_read_cases[] = {
        "rgb-ubyte", "rgb-ushort", "rgb-float", "rgba-ubyte", "rgba-ushort",
        "rgba-float", "transfer-ubyte", "transfer-ushort", "transfer-float",
        "transfer-migrates", "unpack-swap-independent",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(typed_read_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/typed-read/%s", typed_read_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_read_pixels_typed);
    }
    for (unsigned mode = 0; mode < 35; mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/pixels-dma/%s-%s",
            colour_formats[mode / 7], colour_types[mode % 7]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_draw_pixels_native_dma);
    }
    static const char *pixels_dma_cases[] = {
        "ushort-swap", "float-swap-migrates", "uint-transfer",
        "rgb-contiguous-gap", "rgb-strided-skips", "late-source-failure",
        "short-descriptor", "context-changed", "gap-pending-migrates",
        "late-nan-atomic", "float-before-transfer", "negative-zoom",
        "texture-source-replaces-pending",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(pixels_dma_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/pixels-dma/%s", pixels_dma_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(35 + mode),
                     test_draw_pixels_native_dma);
    }
    static const char *raster_texture_cases[] = {
        "pixels-pio", "scalar-pio", "pixels-dma", "scalar-dma", "bitmap-pio",
        "bitmap-dma", "bitmap-inline", "copy-pixels", "transfer-before-texture",
        "alpha-after-texture", "projective-q", "invalid-q",
        "raster-coordinate-retained", "partial-pio-migrates",
        "pending-dma-migrates", "pending-context-refused", "incomplete-image",
        "bitmap-transfer-independent",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(raster_texture_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/raster-texture/%s",
            raster_texture_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_native_raster_texture);
    }
    static const char *scalar_pixels_cases[] = {
        "byte", "short", "ushort", "int", "uint", "float",
        "ushort-swapped", "float-swapped-migrates", "signed-before-bias",
        "float-before-transfer", "float-alpha-precision", "late-nan-atomic",
        "uint-before-transfer",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(scalar_pixels_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/scalar-pixels/%s",
            scalar_pixels_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_draw_pixels_scalar_pio);
    }
    static const char *raw_pixels_cases[] = {
        "rgb-row-gap", "luminance-phase", "luminance-alpha-strided",
        "alpha-transfer", "unpack-skips", "skips-partial-migrates",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(raw_pixels_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/raw-pixels/%s", raw_pixels_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                     test_draw_pixels_raw_pio);
    }
    static const char *draw_pixels_cases[] = {
        "pio", "pio-padded", "dma", "dma-padded", "partial-migrates",
        "nonfinite-zoom-refused", "transfer-refused", "imaging-refused",
        "colour-matrix-refused", "begin-refused", "split-refused",
        "scalar-refused", "dma-migrates", "nonfinite-zoom-migrates",
        "no-context", "incomplete-texture", "zoom-default-restored",
        "partial-context-aborted", "pio-contiguous-padded",
        "dma-strided-rowlength", "malformed-dimensions",
        "unknown-format", "malformed-source-pad", "malformed-layout",
        "contiguous-partial-migrates",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(draw_pixels_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/draw-pixels/%s", draw_pixels_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode), test_draw_pixels);
    }
    add_data_case("sn0/origin2000/kona-3d/colour-matrix/native",
                  GINT_TO_POINTER(0), test_native_colour_matrix);
    add_data_case("sn0/origin2000/kona-3d/colour-matrix/migrates",
                  GINT_TO_POINTER(1), test_native_colour_matrix);
    add_case("sn0/origin2000/kona-3d/native-error/state",
             test_native_error_state);
    add_case("sn0/origin2000/kona-3d/native-error/failed-reply",
             test_native_error_failed_reply);
    add_data_case("sn0/origin2000/kona-3d/native-error/migrates",
                  GINT_TO_POINTER(0), test_native_error_migrates);
    add_data_case("sn0/origin2000/kona-3d/native-error/partial-migrates",
                  GINT_TO_POINTER(1), test_native_error_migrates);
    add_data_case("sn0/origin2000/kona-3d/texture/interior/native",
                  GINT_TO_POINTER(0), test_texture_native_interior);
    add_data_case("sn0/origin2000/kona-3d/texture/interior/migrates",
                  GINT_TO_POINTER(1), test_texture_native_interior);
    static const char *one_d_cases[] = {
        "pio-byte", "dma-byte", "pio-halfword", "dma-halfword", "ignore-t",
        "migrates", "restore-enabled", "restore-disabled", "targets-refused",
        "dimensions-refused", "partial-migrates",
    };

    for (unsigned mode = 0; mode < ARRAY_SIZE(one_d_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/1d/%s", one_d_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_texture_native_1d);
    }
    static const char *depth_texture_cases[] = {
        "pio", "dma", "partial-pio-migrates", "pending-dma-migrates",
        "complete-pio-migrates", "dma-captured-depth-transfer",
        "irrelevant-color-transfer", "late-nan-atomic", "unsigned-byte",
        "unsigned-short-swap", "float-before-depth-transfer", "one-dimensional",
        "gequal", "ambient", "linear-ambient", "comparison-refused",
        "luminance-collision-refused", "reference-range-refused",
        "comparison-refusal-migrates",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(depth_texture_cases); mode++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/kona-3d/texture/depth/%s",
            depth_texture_cases[mode]);

        add_data_case(path, GUINT_TO_POINTER(mode),
                      test_depth_texture_pixels);
    }
    qts = qtest_init(MACHINE_ARGS);
    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
