/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA context swap and GL make-current qtests.
 *
 * Covers the command-page packets a context swap and a GL client's first
 * make-current send: the restore of a context that has run before and the
 * state packets that follow it, and the texture, pixel-state, viewport and
 * scissor commands of the GL client's set-up block. Each case fails on a
 * model without the behaviour for the reason its comment gives.
 *
 * The fixture is sn0-origin2000-kona-bringup-test.c's: KONA as a -device
 * at the empty slot IO4, crossbow0's widget 11, with fabric-hub-widget=9
 * so the board's writes reach node 0's memory.
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

#define REG_INT_CAUSE       KONA_REG(0x800238)
#define REG_MAILBOX         KONA_REG(0x800240)
#define REG_KICK            KONA_REG(0xc00108)
#define REG_COMMAND         KONA_REG(0xc10000)
#define REG_COMMAND_DATA    KONA_REG(0xc1000c)
#define REG_PORT_ADDR       KONA_REG(0x8003e8)
#define REG_PORT_DATA       KONA_REG(0x8003f0)
/* The slot store that opens token t: offset 0xc + 0x20 * t. */
#define REG_COMMAND_SLOT(t) KONA_REG(0xc1000c + 0x20 * (uint64_t)(t))

#define INT_CAUSE_MAILBOX   0x10
#define INT_CAUSE_VIDEO     0x8

#define REG_RETRACE_TAG     KONA_REG(0x800248)
#define REG_INT_DEST_UPPER  KONA_REG(0x000034)
#define REG_INT_DEST_LOWER  KONA_REG(0x00003c)
#define REG_INT_VECTORS     KONA_REG(0x00005c)

/*
 * kona_reset_adaptor's values in the traced run: the general vector 0x10
 * and the geometry-engine vector 0x11, to node 0's Hub at widget 9.
 */
#define KERNEL_INT_VECTORS    0x3e2210
#define KERNEL_INT_DEST_UPPER 0x98000
#define KERNEL_INT_DEST_LOWER 0x1800090

/*
 * Virtual time from one retrace to just past the next: the model's 60 Hz
 * field is 16.67 ms. Stepping 20 ms from a point between retraces crosses
 * exactly one when the step starts within 13.3 ms after a retrace.
 */
#define RETRACE_STEP_NS     (20 * 1000 * 1000)

#define REG_MAPRAM(i)       KONA_REG(0x100000 + 4 * (uint64_t)(i))

/*
 * Node 0 guest RAM: the reason page, two contexts' return pages and save
 * areas, and a base for more of each. RETURN_PAGE doubles as a process's
 * own return page.
 */
#define REASON_PAGE         UINT64_C(0x300000)
#define RETURN_PAGE         UINT64_C(0x320000)
#define RETURN_A            RETURN_PAGE
#define RETURN_B            UINT64_C(0x330000)
#define SAVE_A              UINT64_C(0x340000)
#define SAVE_B              UINT64_C(0x360000)
#define SAVE_BASE           UINT64_C(0x800000)
#define RETURN_BASE         UINT64_C(0x500000)

/*
 * The device address the driver gives a save area in the traced run,
 * 0x14000 (page word 0x14, offset word 0x9000), translated through map
 * RAM entry 5, which covers 0x14000-0x17fff.
 */
#define SAVE_DEVICE_PAGE    0x14
#define SAVE_DEVICE_ENTRY   5

/* The model's save-area words: its own marker, and the 0x172 token. */
#define SAVE_MARKER         0x4b4f4e41
#define SAVE_TOKEN          0x172

/* The texture table's data words after its slot word. */
#define TEX_TABLE_WORDS     33

#define MACHINE_ARGS \
    "-M origin2000,nodes=1,x-uncatalogued-graphics=on -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

/* The one-node machine is small enough to migrate quickly as it is. */
#define MIGRATION_ARGS MACHINE_ARGS

/*
 * A fresh instance tracing every sgi_kona event and logging guest errors
 * and unimplemented accesses to its own file. The caller quits it and
 * reads the log with kona_log_read(), which also removes the file.
 */
static QTestState *kona_init_logged_args(const char *args, char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-context-XXXXXX.log",
                           g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    return qtest_initf("%s -trace enable=sgi_kona_* -d guest_errors,unimp"
                       " -D %s", args, *log);
}

static QTestState *kona_init_logged(char **log)
{
    return kona_init_logged_args(MACHINE_ARGS, log);
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

/* One command on the +0xc10000 port, with no kicks and no sync word. */
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

/* A 0x1f2 or 0x1fd page word and tagged offset word for a physical page. */
static uint32_t physical_page_word(uint64_t addr)
{
    return 0x10000000 | (uint32_t)(addr >> 12);
}

static uint32_t tagged_offset_word(uint64_t addr)
{
    return 0x9000 | (uint32_t)(addr & 0xfff);
}

/* The X server context's 0x1f2 for a host physical return page. */
static void kona_register_return_page(QTestState *s, uint64_t page)
{
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(page),
                                      tagged_offset_word(page), 0x4000 }, 3);
}

/* KONAstart's 0x1f1 for a host physical reason page. */
static void kona_register_reason_page(QTestState *s, uint64_t page)
{
    kona_command_kicked(s, 0x1f1,
                        (uint32_t[]){ physical_page_word(page),
                                      tagged_offset_word(page), 0x4000 }, 3);
}

/*
 * KONApcxswap's first activation of a context: 0x1fd with the save area's
 * page word, tagged offset word and 64 KiB, then 0x1f2 with the return
 * page, each in its own bracket.
 */
static void context_first(QTestState *s, uint32_t save_page,
                          uint32_t save_offset, uint64_t return_page)
{
    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ save_page, save_offset, 0x10000 }, 3);
    kona_register_return_page(s, return_page);
}

/* The same with a physical save area. */
static void context_first_physical(QTestState *s, uint64_t save,
                                   uint64_t return_page)
{
    context_first(s, physical_page_word(save), tagged_offset_word(save),
                  return_page);
}

/*
 * KONApcxswap's deactivate of the outgoing context: 0x1fc and its index,
 * with no closing kick, as the driver sleeps there.
 */
static void context_deactivate(QTestState *s, uint32_t index)
{
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, 0x1fc);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, index);
}

/*
 * A restore as KONApcxswap and kona_act_pcx send it, with a context that
 * registered no pool: 0x1f9 with word 0 of the save area at host address
 * `save`, the save area's page word and tagged offset word and the
 * sentinel; 0x1ed 0 0x9000 0; and the last packet, whose token is word 2
 * of the save area and whose one word is 0.
 */
static void context_restore(QTestState *s, uint64_t save, uint32_t save_page,
                            uint32_t save_offset)
{
    kona_command_kicked(s, 0x1f9,
                        (uint32_t[]){ guest_word(s, save), save_page,
                                      save_offset, 0xedededed }, 4);
    kona_command_kicked(s, 0x1ed, (uint32_t[]){ 0, 0x9000, 0 }, 3);
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, guest_word(s, save + 8));
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, 0);
    qtest_writel(s, REG_KICK, 0);
}

static void context_restore_physical(QTestState *s, uint64_t save)
{
    context_restore(s, save, physical_page_word(save),
                    tagged_offset_word(save));
}

/* The X initialise, whose echo of its three words lands on the page. */
static void x_initialise(QTestState *s, uint32_t width)
{
    kona_command(s, 0x128, (uint32_t[]){ width, 1024, 3 }, 3);
}

/*
 * A texture table as the IRIX client library sends it: 0x17e and
 * the slot word, then the 33 table words as sixteen doubleword stores and
 * one word store.
 */
static void gl_tex_table(QTestState *s, uint32_t slot, const uint32_t *words)
{
    qtest_writel(s, REG_COMMAND, 0x17e);
    qtest_writel(s, REG_COMMAND, slot);
    for (unsigned i = 0; i + 1 < TEX_TABLE_WORDS; i += 2) {
        qtest_writeq(s, REG_COMMAND,
                     ((uint64_t)words[i] << 32) | words[i + 1]);
    }
    qtest_writel(s, REG_COMMAND, words[TEX_TABLE_WORDS - 1]);
}

/*
 * Table words that would each misframe the stream if taken as fresh
 * words: the sync probe 1 9 0, a reason post 0x1f8 0x103, the context
 * deactivate 0x1fc, the X initialise 0x128, and the stale word the traced
 * run's slot 2 ended with.
 */
static void tex_table_words(uint32_t *words, uint32_t seed)
{
    static const uint32_t hostile[] = {
        1, 9, 0, 0x1f8, 0x103, 0x1fc, 1, 0x128, 0x17e, 1, 9, 0,
    };

    for (unsigned i = 0; i < TEX_TABLE_WORDS; i++) {
        words[i] = i < ARRAY_SIZE(hostile) ? hostile[i] : seed + i;
    }
    words[TEX_TABLE_WORDS - 1] = 0xf84848f8;
}

/*
 * True if the log shows a word taken off the stream as unframed. The
 * shared GL layer's own "sgi-gl:" messages, such as a draw with no window
 * dropped, are about drawing, not framing.
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
            strstr(*l, "unsupported textport selector")) {
            return true;
        }
    }
    return false;
}

/*
 * The log up to the end of the line that holds `needle`, which must be
 * there. A list call's own words come after its trace line: an empty
 * list buffer's zeros are dropped as stray words, which says nothing
 * about how the call itself framed.
 */
static char *log_through(const char *found, const char *needle)
{
    const char *at = strstr(found, needle);

    g_assert_nonnull(at);
    return g_strndup(found, strchr(at, '\n') + 1 - found);
}

/*
 * A swap back to a context that has run before, as a traced guest run
 * saw it: 0x1f9 with save-area word 0, the
 * save area's page word and tagged offset word and the sentinel;
 * kona_mem_reserve_func's 0x1e4; kona_dmapool_update_pipe's 0x1ed for no
 * pool, which registers none; kona_act_pcx's 0x1f3, 0x1ee and 0x1f4,
 * each bracketed; and its last packet, whose token comes from save-area
 * word 2, sent here as 0x172. Each token is followed by exactly its count
 * of words shaped to misframe the stream if the count is wrong: one too
 * few leaves a word that is dropped or opens a packet of its own, and one
 * too many takes the next token as an argument, so its trace line goes
 * missing. Each command frames whole with its first three words and count
 * traced, no word is dropped, and the textport answers its sync probe
 * afterwards.
 */
static void test_swap_packets_framed(void)
{
    static const uint32_t records[11] = {
        1, 9, 0, 0x1f8, 0x103, 0x55000005, 0x55000006, 0x55000007,
        0x55000008, 0x55000009, 0x5500000a,
    };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    kona_command_kicked(s, 0x1f9,
                        (uint32_t[]){ 0x31302e39, 0x14, 0x9000, 0xedededed },
                        4);
    kona_command_kicked(s, 0x1e4, (uint32_t[]){ 2, 9, 0, 1 }, 4);
    kona_command_kicked(s, 0x1ed, (uint32_t[]){ 0x33000000, 0x9000, 1 }, 3);
    kona_command_kicked(s, 0x1f3, records, 11);
    kona_command_kicked(s, 0x1ee, (uint32_t[]){ 1, 9, 0x44000000 }, 3);
    kona_command_kicked(s, 0x1f4, (uint32_t[]){ 0x66000000, 0xffffffff }, 2);
    qtest_writel(s, REG_COMMAND, 0x172);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, 0);
    qtest_writel(s, REG_KICK, 0);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0x1f9 args 0x31302e39 0x14 0x9000"
                                   " argc 4\n"));
    g_assert_nonnull(strstr(found, "token 0x1ed args 0x33000000 0x9000 0x1"
                                   " argc 3\n"));
    g_assert_nonnull(strstr(found, "token 0x1f3 args 0x1 0x9 0x0 argc 11\n"));
    g_assert_nonnull(strstr(found, "token 0x1ee args 0x1 0x9 0x44000000"
                                   " argc 3\n"));
    g_assert_nonnull(strstr(found, "token 0x1f4 args 0x66000000 0xffffffff"
                                   " 0x0 argc 2\n"));
    g_assert_nonnull(strstr(found, "token 0x172 args 0x0 0x0 0x0 argc 1\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_kernel_command "),
                     ==, 7);
    g_assert_nonnull(strstr(found, "sgi_kona_context_activate token 0x1f9"
                                   " save area 0x14000 "));
    g_assert_nonnull(strstr(found, "sgi_kona_dma_pool base 0x33000000000"
                                   " length 0x1 registered=0"));
    g_assert_nonnull(strstr(found, "sgi_kona_gl_window token 0x1f3 slot 64"
                                   " (1, 9) "));
    g_assert_nonnull(strstr(found, "sgi_kona_gl_window token 0x1ee slot 64"
                                   " (1, 9) "));
    g_assert_nonnull(strstr(found, "token 0x1f4 argc 2 consumed"));
}

/*
 * The GL client's set-up block, in the traced order with words shaped to
 * misframe on a wrong count: 0x177, 0x1d7 twice, 0x17e with its slot word
 * and 33 table words, sixteen of them in pairs through doubleword stores,
 * then a second table, 0xee, 0x1ce, 0xe3, 0xc and 0xe4. Every command
 * frames whole, each table's 34 words are consumed as one command, no
 * word is dropped and the textport answers its sync probe afterwards.
 */
static void test_make_current_stream_framed(void)
{
    uint32_t slot0[TEX_TABLE_WORDS], slot1[TEX_TABLE_WORDS];
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    tex_table_words(slot0, 0x3fcfe3f4);
    tex_table_words(slot1, 0x20080200);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    kona_command(s, 0x1d7, (uint32_t[]){ 1, 9, 0xffffffff, 0x1d000000 }, 4);
    kona_command(s, 0x1d7, (uint32_t[]){ 1, 0, 0xffffffff, 0x1d000001 }, 4);
    gl_tex_table(s, 0, slot0);
    gl_tex_table(s, 1, slot1);
    kona_command(s, 0xee, (uint32_t[]){ 0xee000000 }, 1);
    kona_command(s, 0x1ce, (uint32_t[]){ 0xce000000 }, 1);
    kona_command(s, 0xe3, (uint32_t[]){ 1, 9, 1280, 0xe3000000 }, 4);
    kona_command(s, 0xc, (uint32_t[]){ 1, 9, 1280, 0x0c000000 }, 4);
    kona_command(s, 0xe4, (uint32_t[]){ 0x501 }, 1);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0x177 args 0x77000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x1d7 args 0x1 0x9 0xffffffff"
                                   " argc 4\n"));
    g_assert_nonnull(strstr(found, "token 0x1d7 args 0x1 0x0 0xffffffff"
                                   " argc 4\n"));
    g_assert_nonnull(strstr(found, "token 0x17e args 0x0 0x0 0x0 argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x17e args 0x1 0x0 0x0 argc 1\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_gl_tex_table "), ==,
                     2);
    /* Both tables are kept; the commands after them show their framing. */
    g_assert_cmpuint(count_occurrences(found, "invalid texture filter table"),
                     ==, 0);
    g_assert_nonnull(strstr(found, "token 0xee args 0xee000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x1ce args 0xce000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0xe3 args 0x1 0x9 0x500 argc 4\n"));
    g_assert_nonnull(strstr(found, "token 0xc args 0x1 0x9 0x500 argc 4\n"));
    g_assert_nonnull(strstr(found, "token 0xe4 args 0x501 0x0 0x0 argc 1\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_kernel_command "),
                     ==, 10);
}

/*
 * The GL scissor and the textport's graphics info share 0xc. A bare 0xc
 * with four words is the scissor and queues no mailbox reply; 0xc after
 * the sync word 1 is still the graphics info, whose five-word reply ends
 * with the screen size.
 */
static void test_scissor_beside_graphics_info(void)
{
    static const uint32_t info[] = { 0, 0, 0, 1280, 1024 };
    QTestState *s = qtest_init(MACHINE_ARGS);

    kona_command(s, 0xc, (uint32_t[]){ 1, 9, 0, 1 }, 4);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==, 0);
    kona_command(s, 1, (uint32_t[]){ 0xc, 0 }, 2);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==,
                    INT_CAUSE_MAILBOX);
    for (size_t i = 0; i < ARRAY_SIZE(info); i++) {
        g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX,
                        ==, INT_CAUSE_MAILBOX);
        g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, info[i]);
        qtest_writel(s, REG_MAILBOX, 0);
        qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_MAILBOX);
    }
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);
}

/*
 * A slot store arriving while a texture table still takes its data ends
 * the table, says how many words it lacked, and opens its own command:
 * here the X initialise, whose echo lands on the return page.
 */
static void test_tex_table_cut_by_slot_store(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, RETURN_PAGE, 0);
    kona_register_return_page(s, RETURN_PAGE);
    kona_command(s, 0x17e, (uint32_t[]){ 2, 0x11, 0x22, 0x33, 0x44, 0x55 },
                 6);
    qtest_writel(s, REG_COMMAND_SLOT(0x128), 1280);
    qtest_writel(s, REG_COMMAND_DATA, 1024);
    qtest_writel(s, REG_COMMAND_DATA, 3);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 1280);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "while token 0x17e still needs 28 words;"
                                   " partial command dropped"));
    g_assert_null(strstr(found, "token 0x17e argc 34 consumed"));
}

/*
 * A texture table caught mid-data by a migration finishes on the
 * destination: the words still to come, which start with the sync probe
 * 1 9 0, are consumed there, and the textport answers its own sync probe
 * after them. A destination that lost the open table would answer the
 * first probe on the mailbox.
 */
static void test_migration_mid_tex_table(void)
{
    uint32_t words[TEX_TABLE_WORDS];
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    tex_table_words(words, 0x70000000);
    words[10] = 1;
    words[11] = 9;
    words[12] = 0;
    qtest_writel(from, REG_COMMAND, 0x17e);
    qtest_writel(from, REG_COMMAND, 0);
    for (unsigned i = 0; i < 10; i++) {
        qtest_writel(from, REG_COMMAND, words[i]);
    }

    sgi_migrate_stopped(from, uri);

    for (unsigned i = 10; i < TEX_TABLE_WORDS; i++) {
        qtest_writel(to, REG_COMMAND, words[i]);
    }
    g_assert_cmphex(qtest_readl(to, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==, 0);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * 0x1fc switches the context the last 0x1fd named out: before reason
 * 0x101 goes to the reason page, word 0 of that context's save area holds
 * the model's marker and word 2 the token 0x172, and words 1 and 3 are
 * untouched. The save area here is physical, bit 28 of the page word set.
 * A model that posts 0x101 without writing leaves the kernel to restore
 * the context with whatever the buffer held, as the traced run did.
 */
static void test_deactivate_writes_save_area(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *save0 = NULL, *save2 = NULL, *reason = NULL;
    const char *w0, *w2, *post;
    QTestState *s = kona_init_logged(&log);

    for (unsigned i = 0; i < 4; i++) {
        guest_fill_word(s, SAVE_A + 4 * i, 0xa5a5a5a5);
    }
    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    g_assert_cmphex(guest_word(s, SAVE_A), ==, 0xa5a5a5a5);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, SAVE_A), ==, SAVE_MARKER);
    g_assert_cmphex(guest_word(s, SAVE_A + 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(s, SAVE_A + 8), ==, SAVE_TOKEN);
    g_assert_cmphex(guest_word(s, SAVE_A + 12), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    qtest_quit(s);

    found = kona_log_read(log);
    save0 = g_strdup_printf("addr 0x%" PRIx64 " len 0x4\n", SAVE_A);
    save2 = g_strdup_printf("addr 0x%" PRIx64 " len 0x4\n", SAVE_A + 8);
    reason = g_strdup_printf("addr 0x%" PRIx64 " len 0x4\n", REASON_PAGE);
    w0 = strstr(found, save0);
    w2 = strstr(found, save2);
    post = strstr(found, reason);
    g_assert_nonnull(w0);
    g_assert_nonnull(w2);
    g_assert_nonnull(post);
    g_assert_true(w0 < post && w2 < post);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_context_save "), ==,
                     1);
}

/*
 * The kernel switches contexts while a user command may be part-way
 * through its words, as when the X server was preempted inside a pixel
 * write. Its forced, privileged bracket (kick 0x30000) suspends the open
 * command, so 0x1fc and its index frame as a kernel command and the
 * deactivate is answered; restored, the context's remaining words finish
 * the same command. Here a viewport, 0xe3 and four words, is cut after
 * two. A pipe framing one stream takes 0x1fc 1 as the viewport's last
 * two words and never answers the kernel, which kills the pipe.
 */
static void test_deactivate_mid_user_command(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *deactivated;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0xe3, (uint32_t[]){ 1, 9 }, 2);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_restore_physical(s, SAVE_A);
    qtest_writel(s, REG_COMMAND, 1280);
    qtest_writel(s, REG_COMMAND, 0xe3000000);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    deactivated = strstr(found, "sgi_kona_context_deactivate outgoing"
                                " context 1\n");
    g_assert_nonnull(deactivated);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(deactivated, "token 0xe3 args 0x1 0x9 0x500"
                                         " argc 4\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_bracket_suspend "),
                     ==, 1);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_bracket_resume "),
                     ==, 1);
}

/*
 * The suspended command belongs to its context: when another context
 * sends words first, the command is saved with its owner and the new
 * context frames from its own first word; when the owner is restored, its
 * remaining words finish the same command. Context A's viewport is cut
 * after two words, B runs a whole command, then A comes back. A model that
 * kept only one suspended command in place ended A's viewport at B's
 * first word, and A's last two words framed as nothing.
 */
static void test_suspended_command_survives_other_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0xe3, (uint32_t[]){ 1, 9 }, 2);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    guest_fill_word(s, REASON_PAGE, 0);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_restore_physical(s, SAVE_A);
    qtest_writel(s, REG_COMMAND, 1280);
    qtest_writel(s, REG_COMMAND, 0xe3000000);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0x177 args 0x77000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0xe3 args 0x1 0x9 0x500"
                                   " argc 4\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_save "), ==, 1);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_restore "), ==,
                     1);
}

/*
 * A context started afresh by 0x1fd has no command of its own left open:
 * a copy left by an earlier context with the same save area is discarded.
 * A's viewport is cut and saved when B sends words; then A's save area is
 * registered afresh, as for a new process, and its first command frames
 * whole. Restoring the stale copy would take that command's words as the
 * viewport's last two.
 */
static void test_fresh_context_discards_saved_command(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0xe3, (uint32_t[]){ 1, 9 }, 2);
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    context_deactivate(s, 2);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000001 }, 1);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_save "), ==, 1);
    g_assert_null(strstr(found, "sgi_kona_input_restore "));
    g_assert_null(strstr(found, "token 0xe3 args"));
    g_assert_nonnull(strstr(found, "token 0x177 args 0x77000001 0x0 0x0"
                                   " argc 1\n"));
}

/*
 * A suspended GL data run moves with its context too: the polygon
 * stipple, 0xb and three words, has two of its 32 rows when B sends
 * words; when A is restored its remaining 30 rows finish the same run. A
 * model that ended the run at B's first word framed A's rows as nothing.
 */
static void test_suspended_gl_run_survives_other_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0xb, (uint32_t[]){ 0, 0, 0, 0x55555555, 0xaaaaaaaa }, 5);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    guest_fill_word(s, REASON_PAGE, 0);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_restore_physical(s, SAVE_A);
    for (unsigned row = 2; row < 32; row++) {
        qtest_writel(s, REG_COMMAND, row & 1 ? 0xaaaaaaaa : 0x55555555);
    }
    kona_command(s, 0xe3, (uint32_t[]){ 1, 9, 1280, 0xe3000000 }, 4);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0x177 args 0x77000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0xe3 args 0x1 0x9 0x500 argc 4\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_save "), ==, 1);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_restore "), ==,
                     1);
}

/*
 * A GL client's list call, 0x16d with a byte offset and a word count,
 * frames on the kernel framer because the two share a table, but it is a
 * user command: sent without the privileged bit. When the kernel's forced
 * bracket switches the client out between the offset and the count, the
 * call is set aside and saved with its context like any other, B's
 * command frames on its own, and the count A sends once restored finishes
 * the same call. A model that took the forced store as cutting a kernel
 * command dropped the call, logged "forced store ends kernel command",
 * and A's count then framed as a command of its own.
 */
static void test_list_call_split_by_switch(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *framed = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x16d, (uint32_t[]){ 0x6000 }, 1);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    guest_fill_word(s, REASON_PAGE, 0);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_restore_physical(s, SAVE_A);
    qtest_writel(s, REG_COMMAND, 3);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    framed = log_through(found, "sgi_kona_kernel_command token 0x16d"
                                " args 0x6000 0x3 0x0 argc 2\n");
    g_assert_null(strstr(found, "forced store ends"));
    g_assert_false(log_shows_misframing(framed));
    g_assert_nonnull(strstr(found, "token 0x177 args 0x77000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_save "), ==, 1);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_restore "), ==,
                     1);
}

/*
 * The X server's load position, 0x12a and four words, opened by a slot
 * store and continued by plain words, cut by the forced bracket after its
 * second word. Context B's first store is a slot store of its own 0x12a:
 * it saves A's command first, as a plain word does, rather than opening
 * on top of it. Restored, A's last two words finish A's position. A model
 * that ended the command at the forced store logged it and took A's
 * 0x500 and 0x400 as stray words; one whose slot store skipped the save
 * filed B's open command as A's.
 */
static void test_x_command_split_by_switch(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    qtest_writel(s, REG_COMMAND_SLOT(0x12a), 0x10);
    qtest_writel(s, REG_COMMAND_DATA, 0x20);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    qtest_writel(s, REG_COMMAND_SLOT(0x12a), 1);
    qtest_writel(s, REG_COMMAND_DATA, 2);
    qtest_writel(s, REG_COMMAND_DATA, 3);
    qtest_writel(s, REG_COMMAND_DATA, 4);
    guest_fill_word(s, REASON_PAGE, 0);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_restore_physical(s, SAVE_A);
    qtest_writel(s, REG_COMMAND_DATA, 0x500);
    qtest_writel(s, REG_COMMAND_DATA, 0x400);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_null(strstr(found, "forced store ends"));
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "sgi_kona_kernel_command token 0x12a"
                                   " args 0x1 0x2 0x3 argc 4\n"));
    g_assert_nonnull(strstr(found, "sgi_kona_kernel_command token 0x12a"
                                   " args 0x10 0x20 0x500 argc 4\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_save "), ==, 1);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_restore "), ==,
                     1);
}

/*
 * The X server's data tokens hold rectangles for the X rect that ends
 * the run, and the kernel can switch the X server out between the two:
 * the traced distort run sent X data4, then the kernel's 0x1fc, a GL
 * context's commands, the X server's restore and the X rect. Each of the
 * three rectangles is drawn once the rect arrives, with nothing dropped.
 * A run_x_data_split() with `migrate` set moves the machine while B is
 * current and A's run waits, and the destination finishes it.
 */
static const uint32_t x_run_data[] = {
    0x000a0014, 0x001e0028, 0x0032003c, 0x00070008,
};
static const uint32_t x_run_rect[] = { 0x00640064, 0x00050006 };

/* The X initialise and a solid fill GC of index 0x3c, as the fill tests. */
static void x_run_setup(QTestState *s)
{
    static const uint32_t gc[13] = {
        0x3c000, 0x1ffe, 0, 0, 0xf00fff, 0,
        0xc, 1, 1, 6, 0, 1, 1,
    };

    x_initialise(s, 1280);
    kona_command(s, 0x129, gc, ARRAY_SIZE(gc));
}

static void assert_x_run_drawn(const char *found)
{
    g_assert_null(strstr(found, "X data run of"));
    g_assert_null(strstr(found, "box words dropped"));
    g_assert_nonnull(strstr(found, "sgi_kona_x_rect (10, 20) width 30"
                                   " height 40 "));
    g_assert_nonnull(strstr(found, "sgi_kona_x_rect (50, 60) width 7"
                                   " height 8 "));
    g_assert_nonnull(strstr(found, "sgi_kona_x_rect (100, 100) width 5"
                                   " height 6 "));
}

static void test_x_data_run_survives_other_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    x_run_setup(s);
    kona_command(s, 0x144, x_run_data, ARRAY_SIZE(x_run_data));
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    guest_fill_word(s, REASON_PAGE, 0);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0x152, x_run_rect, ARRAY_SIZE(x_run_rect));
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "sgi_kona_x_run_held 4 X run words kept"
                                   " past 0x1fc for context 0\n"));
    assert_x_run_drawn(found);
}

/* A clip list split the same way loads whole when its X load clip comes. */
static void test_x_clip_run_survives_other_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    x_initialise(s, 1280);
    /* Two rectangles: the count and the first in X bdata4, the rest. */
    kona_command(s, 0x145, (uint32_t[]){ 2, 0, 0, 10 }, 4);
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    context_deactivate(s, 2);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0x12b, (uint32_t[]){ 10, 20, 20, 40, 40 }, 5);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_null(strstr(found, "clip list words dropped"));
    g_assert_nonnull(strstr(found, "sgi_kona_x_clip rectangles 2 last"
                                   " (20, 20) to (40, 40)\n"));
}

/*
 * The owner's own non-X command still ends its run, as before; and a
 * context started afresh in the owner's table entry drops the run it
 * left, logged, rather than handing it to the new context.
 */
static void test_x_data_run_owner_rules(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    x_run_setup(s);
    kona_command(s, 0x144, x_run_data, ARRAY_SIZE(x_run_data));
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    kona_command(s, 0x144, x_run_data, ARRAY_SIZE(x_run_data));
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x152, x_run_rect, ARRAY_SIZE(x_run_rect));
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "X data run of 4 words dropped before"
                                   " 0x177\n"));
    g_assert_nonnull(strstr(found, "started afresh with an X data run of 4"
                                   " words open; the run is dropped\n"));
    g_assert_null(strstr(found, "sgi_kona_x_rect (10, 20) "));
    g_assert_nonnull(strstr(found, "sgi_kona_x_rect (100, 100) width 5"
                                   " height 6 "));
}

/*
 * Only the X server's context is expected to send X tokens. If another
 * context sends one while A's run is held, the shared run takes it, and
 * the log says so: here B's X data2 adds to A's run and B's X rect ends
 * it, drawing A's rectangles under B.
 */
static void test_x_run_taken_by_other_context_logged(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    x_run_setup(s);
    kona_command(s, 0x144, x_run_data, ARRAY_SIZE(x_run_data));
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x142, (uint32_t[]){ 0x00c800c8, 0x00030004 }, 2);
    kona_command(s, 0x152, x_run_rect, ARRAY_SIZE(x_run_rect));
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "context 0's X run of 4 words is taken by"
                                   " token 0x142 while context 1 is"
                                   " current\n"));
    g_assert_nonnull(strstr(found, "context 0's X run of 6 words is taken by"
                                   " token 0x152 while context 1 is"
                                   " current\n"));
    g_assert_nonnull(strstr(found, "sgi_kona_x_rect (200, 200) width 3"
                                   " height 4 "));
}

/* The waiting run and its owner migrate with B current. */
static void test_migration_of_x_data_run(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = kona_init_logged_args(dest_args, &log);

    context_first_physical(from, SAVE_A, RETURN_A);
    qtest_writel(from, REG_KICK, 0);
    x_run_setup(from);
    kona_command(from, 0x144, x_run_data, ARRAY_SIZE(x_run_data));
    context_deactivate(from, 1);
    context_first_physical(from, SAVE_B, RETURN_B);
    qtest_writel(from, REG_KICK, 0);

    sgi_migrate_stopped(from, uri);

    kona_command(to, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    context_deactivate(to, 2);
    context_restore_physical(to, SAVE_A);
    kona_command(to, 0x152, x_run_rect, ARRAY_SIZE(x_run_rect));
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);

    found = kona_log_read(log);
    assert_x_run_drawn(found);
}

/*
 * A kernel command is still the kernel's: one the forced bracket cuts
 * short is dropped and logged as before, and the kernel's next packet
 * frames whole. Here a reason-page registration, 0x1f1 and three words,
 * loses its last two.
 */
static void test_privileged_command_cut_by_force(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, 0x1f1);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, physical_page_word(REASON_PAGE));
    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "forced store ends kernel"
                                              " command 0x1f1 "), ==, 1);
    g_assert_null(strstr(found, "sgi_kona_bracket_suspend "));
}

/*
 * A list call the forced bracket set aside migrates in both places it can
 * be: still suspended, before any other context has sent words, and
 * saved with its context after one has. On the destination A is restored
 * and its count finishes the call, which only a destination holding the
 * offset can frame as 0x16d 0x6000 3.
 */
static void test_migration_of_kernel_port_command(gconstpointer opaque)
{
    bool saved = GPOINTER_TO_INT(opaque);
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *framed = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = kona_init_logged_args(dest_args, &log);

    kona_register_reason_page(from, REASON_PAGE);
    context_first_physical(from, SAVE_A, RETURN_A);
    qtest_writel(from, REG_KICK, 0);
    kona_command(from, 0x16d, (uint32_t[]){ 0x6000 }, 1);
    context_deactivate(from, 1);
    if (saved) {
        context_first_physical(from, SAVE_B, RETURN_B);
        qtest_writel(from, REG_KICK, 0);
        kona_command(from, 0x177, (uint32_t[]){ 0x77000000 }, 1);
        context_deactivate(from, 2);
    }

    sgi_migrate_stopped(from, uri);

    context_restore_physical(to, SAVE_A);
    qtest_writel(to, REG_COMMAND, 3);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);

    found = kona_log_read(log);
    framed = log_through(found, "sgi_kona_kernel_command token 0x16d"
                                " args 0x6000 0x3 0x0 argc 2\n");
    g_assert_false(log_shows_misframing(framed));
    g_assert_cmpuint(count_occurrences(found, saved ?
                                       "sgi_kona_input_restore " :
                                       "sgi_kona_bracket_resume "), ==, 1);
}

/*
 * A saved command migrates with its context: A's line-distance query 0xee
 * is cut before its one word, B's command saves it, and the machine
 * migrates. On the destination A is restored and its word arrives; the
 * query is answered on A's return page. A destination that lost the
 * saved command takes the word as nothing and never answers.
 */
static void test_migration_of_saved_command(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    guest_fill_word(from, REASON_PAGE, 0);
    guest_fill_word(from, RETURN_A, 0);
    kona_register_reason_page(from, REASON_PAGE);
    context_first_physical(from, SAVE_A, RETURN_A);
    qtest_writel(from, REG_KICK, 0);
    qtest_writel(from, REG_COMMAND, 0xee);
    context_deactivate(from, 1);
    context_first_physical(from, SAVE_B, RETURN_B);
    qtest_writel(from, REG_KICK, 0);
    kona_command(from, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    context_deactivate(from, 2);

    sgi_migrate_stopped(from, uri);

    context_restore_physical(to, SAVE_A);
    g_assert_cmphex(guest_word(to, RETURN_A), ==, 0);
    qtest_writel(to, REG_COMMAND, 0);
    g_assert_cmphex(guest_word(to, RETURN_A), !=, 0);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * An EEPROM write (0x1ec 1 0 offset 2 and two words) context A leaves one
 * word short when the kernel switches it out stays A's. Context B's own
 * EEPROM check frames and answers on B's page meanwhile. The saved
 * operation crosses migration, and A's last word on the destination
 * finishes it: status 1 on A's page and both words in the flash, which
 * the indirect port reads at internal 0x800000 plus the flash offset.
 */
static void test_migration_of_saved_flash_op(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    guest_fill_word(from, REASON_PAGE, 0);
    guest_fill_word(from, RETURN_A, 0);
    guest_fill_word(from, RETURN_B, 0);
    kona_register_reason_page(from, REASON_PAGE);
    context_first_physical(from, SAVE_A, RETURN_A);
    qtest_writel(from, REG_KICK, 0);
    kona_command(from, 0x1ec,
                 (uint32_t[]){ 1, 0, 0x200, 2, 0x5a5a1111 }, 5);
    context_deactivate(from, 1);
    context_first_physical(from, SAVE_B, RETURN_B);
    qtest_writel(from, REG_KICK, 0);
    kona_command(from, 0x1ec, (uint32_t[]){ 4, 0 }, 2);
    g_assert_cmphex(guest_word(from, RETURN_B), ==, 1);
    g_assert_cmphex(guest_word(from, RETURN_B + 8), ==, 1);
    g_assert_cmphex(guest_word(from, RETURN_A), ==, 0);
    context_deactivate(from, 2);

    sgi_migrate_stopped(from, uri);

    context_restore_physical(to, SAVE_A);
    g_assert_cmphex(guest_word(to, RETURN_A), ==, 0);
    qtest_writel(to, REG_COMMAND, 0x5a5a2222);
    g_assert_cmphex(guest_word(to, RETURN_A), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_A + 4), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_A + 8), ==, 1);
    qtest_writel(to, REG_PORT_ADDR, 0x800200);
    g_assert_cmphex(qtest_readl(to, REG_PORT_DATA), ==, 0x5a5a1111);
    qtest_writel(to, REG_PORT_ADDR, 0x800204);
    g_assert_cmphex(qtest_readl(to, REG_PORT_DATA), ==, 0x5a5a2222);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * The same through the map RAM, as the driver's own save areas are: 0x1fd
 * 0x14 0x9010 names device address 0x14010, entry 5 maps its 16 KiB page
 * to SAVE_B, and the words land at SAVE_B + 0x10 and + 0x18. A second
 * 0x1fc with no context activated since writes nothing more and says so.
 */
static void test_deactivate_writes_mapped_save_area(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    for (unsigned i = 0; i < 8; i++) {
        guest_fill_word(s, SAVE_B + 4 * i, 0xa5a5a5a5);
    }
    kona_register_reason_page(s, REASON_PAGE);
    qtest_writel(s, REG_MAPRAM(SAVE_DEVICE_ENTRY),
                 0x20000000 | (uint32_t)(SAVE_B >> 12));
    context_first(s, SAVE_DEVICE_PAGE, 0x9010, RETURN_A);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, SAVE_B), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(s, SAVE_B + 0x10), ==, SAVE_MARKER);
    g_assert_cmphex(guest_word(s, SAVE_B + 0x14), ==, 0xa5a5a5a5);
    g_assert_cmphex(guest_word(s, SAVE_B + 0x18), ==, SAVE_TOKEN);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);

    guest_fill_word(s, SAVE_B + 0x10, 0);
    guest_fill_word(s, REASON_PAGE, 0);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, SAVE_B + 0x10), ==, 0);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_context_save "), ==,
                     1);
    g_assert_nonnull(strstr(found, "context deactivate 2 with no context"
                                   " current; no save area written"));
}

/*
 * One return page per context: A's first activation and page, deactivate,
 * B's first activation and page, deactivate, then A restored by 0x1f9 alone,
 * which sends no 0x1f2. The X initialise echo then lands on A's page, not
 * B's, the page the last 0x1f2 named. The restore carries back the words the
 * model left, and its last packet, 0x172 0, frames with nothing dropped.
 * Restoring B then answers on B's page.
 */
static void test_return_page_per_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, RETURN_A, 0);
    guest_fill_word(s, RETURN_B, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_B, RETURN_B);
    context_deactivate(s, 2);
    context_restore_physical(s, SAVE_A);
    x_initialise(s, 1280);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 1280);
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 0);

    context_deactivate(s, 1);
    context_restore_physical(s, SAVE_B);
    x_initialise(s, 640);
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_B + 8), ==, 640);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 1280);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_null(strstr(found, "not the 0x4b4f4e41 the model leaves"));
    g_assert_cmpuint(count_occurrences(found, "token 0x172 args 0x0 0x0 0x0"
                                              " argc 1\n"), ==, 2);
    g_assert_cmpuint(count_occurrences(found, "token 0x1f9 args 0x4b4f4e41 "),
                     ==, 2);
}

/*
 * A restore naming a save area no 0x1fd set up, here after a reset
 * forgot context A, leaves the pipe with no return page: the X initialise
 * echo lands nowhere, neither on A's old page nor on the page a process's
 * own 0x1f2 registered before, and both are logged. A restore carrying a
 * word 0 the model did not leave is logged as well.
 */
static void test_restore_unknown_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *unknown = NULL;
    QTestState *s = kona_init_logged(&log);

    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_system_reset(s);
    guest_fill_word(s, RETURN_A, 0);
    guest_fill_word(s, RETURN_B, 0);
    kona_register_return_page(s, RETURN_B);
    kona_command_kicked(s, 0x1f9,
                        (uint32_t[]){ 0x31302e39, physical_page_word(SAVE_A),
                                      tagged_offset_word(SAVE_A),
                                      0xedededed }, 4);
    x_initialise(s, 1280);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    unknown = g_strdup_printf("context restore names save area 0x%" PRIx64
                              ", which no 0x1fd set up; no return page",
                              (uint64_t)physical_page_word(SAVE_A) << 12);
    g_assert_nonnull(strstr(found, unknown));
    g_assert_nonnull(strstr(found, "context restore carries save-area word"
                                   " 0x31302e39, not the 0x4b4f4e41 the"
                                   " model leaves"));
    g_assert_nonnull(strstr(found, "token 0x128 with no return page"
                                   " registered"));
}

static uint64_t kona_get_uint(QTestState *s, const char *name);
static uint32_t shown_pixel(QTestState *s, uint32_t x, uint32_t y);
static void context_window(QTestState *s, uint32_t x, uint32_t y,
                            bool buffered);

static void context_table_prepare(QTestState *s)
{
    static const uint32_t colours[9][3] = {
        { 0, 0, 0x3f800000 }, { 0, 0x3f800000, 0 },
        { 0, 0x3f800000, 0x3f800000 }, { 0x3f800000, 0, 0 },
        { 0x3f800000, 0, 0x3f800000 },
        { 0x3f800000, 0x3f800000, 0 },
        { 0x3f800000, 0x3f800000, 0x3f800000 },
        { 0, 0, 0 }, { 0x3f000000, 0, 0 },
    };

    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_BASE + 0x90000, RETURN_BASE + 0x24000);
    context_deactivate(s, 9);
    for (unsigned i = 0; i < 9; i++) {
        uint32_t viewport[] = { -13 - i, 17 + i, 31 + i, 43 + i };

        context_first_physical(s, SAVE_BASE + 0x10000 * i,
                               RETURN_BASE + 0x4000 * i);
        context_window(s, 20 + 8 * i, 30, false);
        kona_command(s, 0x1d,
            (uint32_t[]){ colours[i][0], colours[i][1], colours[i][2],
                          0x3f800000 }, 4);
        kona_command(s, 0xe3, viewport, 4);
        kona_command(s, 0xcf,
            (uint32_t[]){ 0x3fd00000 + 0x10000 * i, 0,
                          0x3fe80000, 0 }, 4);
        kona_command(s, 0xc9, (uint32_t[]){ 0x800 }, 1);
        viewport[0] += 100;
        kona_command(s, 0xe3, viewport, 4);
        context_deactivate(s, i);
        context_restore_physical(s, SAVE_BASE + 0x90000);
        x_initialise(s, 1280);
        g_assert_cmphex(guest_word(s, RETURN_BASE + 0x24000), ==, 1);
        context_deactivate(s, 9);
    }
}

static void assert_context_table(QTestState *s)
{
    static const uint32_t pixels[] = {
        0xff, 0xff00, 0xffff, 0xff0000, 0xff00ff,
        0xffff00, 0xffffff, 0, 0x800000,
    };

    for (unsigned i = 0; i < 9; i++) {
        uint64_t page = RETURN_BASE + 0x4000 * i;
        uint32_t viewport[] = { -13 - i, 17 + i, 31 + i, 43 + i };

        for (unsigned j = 0; j < 10; j++) {
            guest_fill_word(s, RETURN_BASE + 0x4000 * j, 0);
        }
        context_restore_physical(s, SAVE_BASE + 0x10000 * i);
        kona_command(s, 0xc8, (uint32_t[]){ 0 }, 1);
        guest_fill_word(s, page + 4, 0x12345678);
        guest_fill_word(s, page + 24, 0x87654321);
        kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
        g_assert_cmphex(guest_word(s, page), ==, 1);
        for (unsigned n = 0; n < 4; n++) {
            g_assert_cmphex(guest_word(s, page + 8 + 4 * n), ==,
                            viewport[n]);
        }
        g_assert_cmphex(guest_word(s, page + 4), ==, 0x12345678);
        g_assert_cmphex(guest_word(s, page + 24), ==, 0x87654321);
        for (unsigned j = 0; j < 10; j++) {
            if (j != i) {
                g_assert_cmphex(guest_word(s, RETURN_BASE + 0x4000 * j), ==, 0);
            }
        }
        kona_command(s, 0x4c, (uint32_t[]){ 0xb70 }, 1);
        g_assert_cmphex(guest_word(s, page + 8), ==,
                        0x3fd00000 + 0x10000 * i);
        g_assert_cmphex(guest_word(s, page + 12), ==, 0);
        g_assert_cmphex(guest_word(s, page + 16), ==, 0x3fe80000);
        g_assert_cmphex(guest_word(s, page + 20), ==, 0);
        kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
        g_assert_cmphex(shown_pixel(s, 20 + 8 * i, 30), ==, pixels[i]);
        g_assert_cmphex(shown_pixel(s, 19 + 8 * i, 30), ==, 0);
        g_assert_cmphex(shown_pixel(s, 24 + 8 * i, 30), ==, 0);
        context_deactivate(s, i);
    }
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 10);
}

/* Nine GL keys and an interleaved X key retain pages, state and windows. */
static void test_context_table_retains_ten(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    context_table_prepare(s);
    assert_context_table(s);
    qtest_quit(s);
}

static uint64_t kona_get_uint(QTestState *s, const char *name)
{
    QDict *reply = qtest_qmp(s,
        "{ 'execute': 'qom-get', 'arguments': { 'path': "
        "'/machine/module0/kona0', 'property': %s } }", name);
    uint64_t value = qdict_get_int(reply, "return");

    qobject_unref(reply);
    return value;
}

static char *kona_get_string(QTestState *s, const char *name)
{
    QDict *reply = qtest_qmp(s,
        "{ 'execute': 'qom-get', 'arguments': { 'path': "
        "'/machine/module0/kona0', 'property': %s } }", name);
    char *value = g_strdup(qdict_get_str(reply, "return"));

    qobject_unref(reply);
    return value;
}

static void kona_set_uint(QTestState *s, const char *name, uint32_t value)
{
    qtest_qmp_assert_success(s,
        "{ 'execute': 'qom-set', 'arguments': { 'path': "
        "'/machine/module0/kona0', 'property': %s, 'value': %u } }",
        name, value);
}

static uint32_t shown_pixel(QTestState *s, uint32_t x, uint32_t y)
{
    kona_set_uint(s, "x-test-pixel-x", x);
    kona_set_uint(s, "x-test-pixel-y", y);
    return kona_get_uint(s, "x-test-pixel");
}

/* A textport command from a user context: the sync word, then the command. */
static void textport_command(QTestState *s, uint32_t cmd, const uint32_t *args,
                             unsigned argc)
{
    qtest_writel(s, REG_COMMAND, 1);
    qtest_writel(s, REG_COMMAND, cmd);
    for (unsigned k = 0; k < argc; k++) {
        qtest_writel(s, REG_COMMAND, args[k]);
    }
}

/* The texel probe: the bound texture's base level at (x, y). */
static uint32_t texture_pixel(QTestState *s, uint32_t x, uint32_t y)
{
    kona_set_uint(s, "x-test-pixel-x", x);
    kona_set_uint(s, "x-test-pixel-y", y);
    return kona_get_uint(s, "x-test-texture-pixel");
}

/*
 * A 2 by 2 RGBA texel load with load identifier 5, bound first, then its
 * header and the first two of its four texel words.
 */
static const uint32_t gl_run_texels[4] = {
    0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
};

static void gl_run_texture_start(QTestState *s)
{
    uint32_t record[22] = { 0x43, 0x101010, 256u << 10 };

    kona_command(s, 0x1d5, (uint32_t[]){ 5, (2u << 28) | (41 << 12),
                                         0x30 << 14, 0x11, 1 }, 5);
    kona_command(s, 0x179, record, 22);
    kona_command(s, 0x174, (uint32_t[]){ 0xde1, 2, 2, 5, 1, 0, 0, 0, 0, 0x30,
                                         5, 0, 0, 1, 1, 0, 0x23, 0x2181, 0,
                                         0 }, 20);
    qtest_writel(s, REG_COMMAND, gl_run_texels[0]);
    qtest_writel(s, REG_COMMAND, gl_run_texels[1]);
}

static void gl_run_texture_finish(QTestState *s)
{
    qtest_writel(s, REG_COMMAND, gl_run_texels[2]);
    qtest_writel(s, REG_COMMAND, gl_run_texels[3]);
    kona_command(s, 0x176, (uint32_t[]){ 5 }, 1);
    for (unsigned k = 0; k < 4; k++) {
        g_assert_cmphex(texture_pixel(s, k % 2, k / 2), ==, gl_run_texels[k]);
    }
}

/*
 * A texel load's buffer moves with its context: A's load is cut after two
 * of its four texel words, B runs a command, and after A is restored its
 * last two words and its load identifier publish all four texels. A
 * model that ended the run at B's first word published nothing.
 */
static void test_suspended_texture_load_survives(void)
{
    QTestState *s = qtest_init(MIGRATION_ARGS);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    gl_run_texture_start(s);
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    context_deactivate(s, 2);
    context_restore_physical(s, SAVE_A);
    gl_run_texture_finish(s);
    assert_textport_in_step(s);
    qtest_quit(s);
}

/* The same with a migration while B is current and A's run is saved. */
static void test_migration_of_saved_gl_run(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    context_first_physical(from, SAVE_A, RETURN_A);
    qtest_writel(from, REG_KICK, 0);
    gl_run_texture_start(from);
    context_deactivate(from, 1);
    context_first_physical(from, SAVE_B, RETURN_B);
    qtest_writel(from, REG_KICK, 0);
    kona_command(from, 0x177, (uint32_t[]){ 0x77000000 }, 1);

    sgi_migrate_stopped(from, uri);

    context_deactivate(to, 2);
    context_restore_physical(to, SAVE_A);
    gl_run_texture_finish(to);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/* The front colour word at X coordinates (x, y), y down from the top. */
static uint32_t front_word(QTestState *s, uint32_t x, uint32_t y)
{
    QDict *reply;
    uint32_t value;

    qtest_qmp_assert_success(s,
        "{ 'execute': 'qom-set', 'arguments': { 'path': "
        "'/machine/module0/kona0', 'property': 'x-test-plane-select', "
        "'value': %" PRIu64 " } }", ((uint64_t)(1023 - y) << 24) | x);
    reply = qtest_qmp(s,
        "{ 'execute': 'qom-get', 'arguments': { 'path': "
        "'/machine/module0/kona0', 'property': 'x-test-plane-word' } }");
    value = qdict_get_int(reply, "return");
    qobject_unref(reply);
    return value;
}

/*
 * The X server's 3 by 2 direct-colour write at (100, 200): a copy of RGB 0
 * under mask 0xffffff, the setup for 32-bit rows, the write and the first
 * three of its six row words. x_write_finish() sends the rest and checks.
 */
static const uint32_t x_write_words[] = {
    0x00112233, 0xff445566, 0x00778899,
    0x00aabbcc, 0x00ddeeff, 0x00010203,
};

static void x_write_start(QTestState *s)
{
    static const uint32_t gc[13] = {
        0, 0x1ffe, 0, 0x1ffe, 0xffffff, 0xffffff,
        0x2804, 0x60001, 1, 6, 8, 0, 0,
    };

    x_initialise(s, 1280);
    kona_command(s, 0x129, gc, ARRAY_SIZE(gc));
    kona_command(s, 0x136, (uint32_t[]){ 0, 2, 2, 8, 3, 2, 0 }, 7);
    kona_command(s, 0x139, (uint32_t[]){ 100, 200 }, 2);
    for (unsigned k = 0; k < 3; k++) {
        qtest_writel(s, REG_COMMAND, x_write_words[k]);
    }
}

static void x_write_finish(QTestState *s)
{
    for (unsigned k = 3; k < 6; k++) {
        qtest_writel(s, REG_COMMAND, x_write_words[k]);
    }
    kona_command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    for (unsigned k = 0; k < 6; k++) {
        g_assert_cmphex(front_word(s, 100 + k % 3, 200 + k / 3) & 0xffffff,
                        ==, x_write_words[k] & 0xffffff);
    }
}

/*
 * An X pixel write stays with its context: A's 3 by 2 direct-colour write
 * at (100, 200) is cut after three of its six words, B runs a command, and
 * after A is restored its last three words and the finish draw all six.
 * Its rows take only A's words: a model that took B's as rows, or ended
 * the write at B's first word, drew the second row wrong or not at all.
 */
static void test_suspended_x_write_survives_other_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    x_write_start(s);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    guest_fill_word(s, REASON_PAGE, 0);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_restore_physical(s, SAVE_A);
    x_write_finish(s);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0x177 args 0x77000000 0x0 0x0"
                                   " argc 1\n"));
}

/* The same with a migration while B is current and A's write is open. */
static void test_migration_of_suspended_x_write(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    context_first_physical(from, SAVE_A, RETURN_A);
    qtest_writel(from, REG_KICK, 0);
    x_write_start(from);
    context_deactivate(from, 1);
    context_first_physical(from, SAVE_B, RETURN_B);
    qtest_writel(from, REG_KICK, 0);
    kona_command(from, 0x177, (uint32_t[]){ 0x77000000 }, 1);

    sgi_migrate_stopped(from, uri);

    kona_command(to, 0x177, (uint32_t[]){ 0x77000001 }, 1);
    context_deactivate(to, 2);
    context_restore_physical(to, SAVE_A);
    x_write_finish(to);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * An X write fed by DMA takes the next unclaimed DMA packet whichever
 * context is current, logged when its owner is not: A opens a 16 by 4
 * colour-index image by DMA, B is activated and current when the
 * kernel's packet arrives, and the image still lands, with the log line.
 */
static void test_x_dma_for_other_context_logged(void)
{
    static const uint32_t gc[13] = {
        0, 0, 0, 0, 0xf00fff, 0, 0x180c, 0x40001, 1, 6, 5, 1, 1,
    };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);
    uint8_t bytes[64];

    for (unsigned k = 0; k < sizeof(bytes); k++) {
        bytes[k] = k;
    }
    qtest_memwrite(s, UINT64_C(0x400000), bytes, sizeof(bytes));
    qtest_writel(s, REG_MAPRAM(0), 0x20000000 | (0x400000 >> 12));
    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    x_initialise(s, 1280);
    kona_command(s, 0x129, gc, ARRAY_SIZE(gc));
    kona_command(s, 0x136, (uint32_t[]){ 0x10, 0, 1, 8, 16, 4, 0 }, 7);
    kona_command(s, 0x139, (uint32_t[]){ 100, 50 }, 2);
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, 0x1eb);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND, 0);
    qtest_writel(s, REG_COMMAND, 0x9000);
    qtest_writel(s, REG_COMMAND, 64);
    qtest_writel(s, REG_COMMAND, 64);
    qtest_writel(s, REG_COMMAND, 0);
    qtest_writel(s, REG_COMMAND, 0x100);
    qtest_writel(s, REG_KICK, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "X pixel write takes a DMA"
                                       " packet while context"), ==, 1);
    g_assert_nonnull(strstr(found, "sgi_kona_x_image_dma "));
}

/*
 * A textport bitmap moves with its context too, payload and all. A's 32 by
 * 2 bitmap is cut after its first payload word, B runs a command, and A's
 * second word, after A is restored, completes the blit: both rows draw.
 * A model that ended the bitmap at B's first word drew nothing.
 */
static void test_suspended_bitmap_survives_other_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    qtest_writel(s, REG_KICK, 0);
    textport_command(s, 3, (uint32_t[]){ 1, 0xab << 2, 0xcd << 2,
                                         0xef << 2 }, 4);
    textport_command(s, 2, (uint32_t[]){ 1 }, 1);
    textport_command(s, 4, (uint32_t[]){ 300, 400 }, 2);
    textport_command(s, 0xa, (uint32_t[]){ 32, 2, 0, 0, 0, 0 }, 6);
    qtest_writel(s, REG_COMMAND, 0x80000001);
    context_deactivate(s, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_first_physical(s, SAVE_B, RETURN_B);
    qtest_writel(s, REG_KICK, 0);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    guest_fill_word(s, REASON_PAGE, 0);
    context_deactivate(s, 2);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x101);
    context_restore_physical(s, SAVE_A);
    qtest_writel(s, REG_COMMAND, 0x00018000);
    g_assert_cmphex(shown_pixel(s, 300, 400), ==, 0x00abcdef);
    g_assert_cmphex(shown_pixel(s, 301, 400), ==, 0);
    g_assert_cmphex(shown_pixel(s, 331, 400), ==, 0x00abcdef);
    g_assert_cmphex(shown_pixel(s, 315, 401), ==, 0x00abcdef);
    g_assert_cmphex(shown_pixel(s, 316, 401), ==, 0x00abcdef);
    g_assert_cmphex(shown_pixel(s, 300, 401), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0x177 args 0x77000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_save "), ==, 1);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_input_restore "), ==,
                     1);
}

/*
 * A saved textport bitmap migrates with its payload: A's 32 by 2 bitmap is
 * cut after one payload word and saved when B sends words, the machine
 * migrates, and on the destination A's second word completes the blit.
 */
static void test_migration_of_saved_bitmap(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    context_first_physical(from, SAVE_A, RETURN_A);
    qtest_writel(from, REG_KICK, 0);
    textport_command(from, 3, (uint32_t[]){ 1, 0xab << 2, 0xcd << 2,
                                            0xef << 2 }, 4);
    textport_command(from, 2, (uint32_t[]){ 1 }, 1);
    textport_command(from, 4, (uint32_t[]){ 300, 400 }, 2);
    textport_command(from, 0xa, (uint32_t[]){ 32, 2, 0, 0, 0, 0 }, 6);
    qtest_writel(from, REG_COMMAND, 0x80000001);
    context_deactivate(from, 1);
    context_first_physical(from, SAVE_B, RETURN_B);
    qtest_writel(from, REG_KICK, 0);
    kona_command(from, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    context_deactivate(from, 2);

    sgi_migrate_stopped(from, uri);

    context_restore_physical(to, SAVE_A);
    qtest_writel(to, REG_COMMAND, 0x00018000);
    g_assert_cmphex(shown_pixel(to, 331, 400), ==, 0x00abcdef);
    g_assert_cmphex(shown_pixel(to, 316, 401), ==, 0x00abcdef);
    assert_textport_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void context_window(QTestState *s, uint32_t x, uint32_t y,
                            bool buffered)
{
    uint32_t config[10] = { 0, 0xc, 0, buffered };

    kona_command(s, 1, config, 10);
    kona_command(s, 2, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(s, 0x1f3,
        (uint32_t[]){ x, y, 0, 0, x, y, 4, 4, 0, 0xd, 0 }, 11);
    kona_command(s, 0xe3, (uint32_t[]){ 0, 0, 4, 4 }, 4);
    kona_command(s, 0xc, (uint32_t[]){ 0, 0, 4, 4 }, 4);
    kona_command(s, 0x1d,
        (uint32_t[]){ 0, 0, 0x3f800000, 0x3f800000 }, 4);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
}

/* An old accepted retrace may complete its tag, never a replacement window. */
static void test_replaced_context_retrace(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    context_first_physical(s, SAVE_A, RETURN_A);
    context_window(s, 20, 30, true);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 7 }, 1);
    context_first_physical(s, SAVE_A, RETURN_B);
    context_window(s, 40, 50, true);
    g_assert_cmphex(shown_pixel(s, 40, 50), ==, 0);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 7);
    g_assert_cmphex(shown_pixel(s, 40, 50), ==, 0);
    qtest_quit(s);
}

/* Unknown restores do not consume allowance, including at the retained cap. */
static void test_unknown_restore_preserves_allowance(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    for (unsigned i = 0; i < 70; i++) {
        kona_command_kicked(s, 0x1f9,
            (uint32_t[]){ SAVE_MARKER,
                physical_page_word(SAVE_BASE + 0x10000 * i),
                0x9000, 0xedededed }, 4);
    }
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 0);
    context_first_physical(s, SAVE_A, RETURN_A);
    context_first_physical(s, SAVE_A + 32, RETURN_B);
    context_deactivate(s, 1);
    context_restore_physical(s, SAVE_A);
    x_initialise(s, 640);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 640);
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 2);
    qtest_quit(s);
}

static void context_triangle(QTestState *s)
{
    kona_command(s, 0x25, (uint32_t[]){ 0xbd0 }, 1);
    kona_command(s, 0xc0, (uint32_t[]){ 0x1d00 }, 1);
    kona_command(s, 0x63, (uint32_t[]){ 0x3f800000, 0x3f800000, 0 }, 3);
    kona_command(s, 0x5b, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x8c, (uint32_t[]){ 0xbf800000, 0xbf800000 }, 2);
    kona_command(s, 0x8c, (uint32_t[]){ 0x3f800000, 0xbf800000 }, 2);
    kona_command(s, 0x8c, (uint32_t[]){ 0xbf800000, 0x3f800000 }, 2);
    kona_command(s, 0x71, (uint32_t[]){ 0 }, 1);
}

/* Reconstruct the shared host renderer without losing a logical window. */
static void test_context_renderer_reconstruction(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-kona.host-gpu=on");
    uint64_t before;

    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    context_window(s, 20, 30, false);
    context_triangle(s);
    before = kona_get_uint(s, "x-test-gpu-draws");
    g_assert_cmpuint(before, >, 0);
    g_assert_cmphex(shown_pixel(s, 20, 30), ==, 0xffff00);
    g_assert_cmphex(shown_pixel(s, 23, 33), ==, 0xff);
    context_deactivate(s, 0);
    for (unsigned i = 0; i < 9; i++) {
        context_first_physical(s, SAVE_BASE + 0x10000 * i,
                               RETURN_BASE + 0x4000 * i);
        context_deactivate(s, i);
    }
    guest_fill_word(s, RETURN_A, 0);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_A + 16), ==, 4);
    g_assert_cmphex(guest_word(s, RETURN_A + 20), ==, 4);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    context_triangle(s);
    g_assert_cmpuint(kona_get_uint(s, "x-test-gpu-draws"), >, before);
    g_assert_cmphex(shown_pixel(s, 20, 30), ==, 0xffff00);
    g_assert_cmphex(shown_pixel(s, 23, 33), ==, 0xff);
    qtest_quit(s);
}

static void assert_context_stop(QTestState *s)
{
    QDict *status = qtest_qmp_assert_success_ref(s,
        "{ 'execute': 'query-status' }");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
    qobject_unref(status);
}

/* The sixty-fifth distinct key stops visibly without losing earlier records. */
static void test_context_limit(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);
    g_autofree char *before = NULL, *after = NULL, *error = NULL;

    for (unsigned i = 0; i < 64; i++) {
        context_first_physical(s, SAVE_BASE + 0x10000 * i,
                               RETURN_BASE + 0x4000 * i);
        context_deactivate(s, i);
    }
    context_restore_physical(s, SAVE_BASE);
    x_initialise(s, 640);
    g_assert_cmphex(guest_word(s, RETURN_BASE + 8), ==, 640);
    kona_command_kicked(s, 0x1f9,
        (uint32_t[]){ SAVE_MARKER,
            physical_page_word(SAVE_BASE + 0x10000 * 64),
            0x9000, 0xedededed }, 4);
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 64);
    context_first_physical(s, SAVE_BASE, RETURN_A);
    x_initialise(s, 1280);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 1280);
    guest_fill_word(s, RETURN_A, 0);
    guest_fill_word(s, RETURN_B, 0);
    before = kona_get_string(s, "x-test-context-digest");
    context_first_physical(s, SAVE_BASE + 0x10000 * 64, RETURN_B);
    x_initialise(s, 320);
    assert_context_stop(s);
    after = kona_get_string(s, "x-test-context-digest");
    g_assert_cmpstr(after, ==, before);
    error = kona_get_string(s, "x-test-context-error");
    g_assert_nonnull(strstr(error, "command 0x1fd key"));
    g_assert_nonnull(strstr(error, "64 retained, limit 64"));
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 64);
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-current"), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 1280);
    qtest_quit(s);
}

/* Migration retains all ten logical records and their attribute copies. */
static void test_migration_of_contexts(void)
{
    g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    QTestState *from = qtest_init(MIGRATION_ARGS);
    QTestState *to = qtest_initf(MIGRATION_ARGS " -incoming %s", uri);

    g_assert_nonnull(dir);
    context_table_prepare(from);
    sgi_migrate_stopped(from, uri);
    assert_context_table(to);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_migration_unknown_selection(void)
{
    g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    QTestState *from = qtest_init(MIGRATION_ARGS);
    QTestState *to = qtest_initf(MIGRATION_ARGS " -incoming %s", uri);

    context_first_physical(from, SAVE_A, RETURN_A);
    kona_command(from, 0xe3, (uint32_t[]){ 3, 5, 7, 11 }, 4);
    context_deactivate(from, 1);
    context_first_physical(from, SAVE_A + 32, RETURN_B);
    kona_command(from, 0xe3, (uint32_t[]){ 13, 17, 19, 23 }, 4);
    context_deactivate(from, 2);
    kona_command_kicked(from, 0x1f9,
        (uint32_t[]){ SAVE_MARKER, physical_page_word(SAVE_B),
                      0x9000, 0xedededed }, 4);
    guest_fill_word(from, RETURN_A, 0);
    guest_fill_word(from, RETURN_B, 0);
    kona_register_return_page(from, RETURN_BASE);
    guest_fill_word(from, RETURN_BASE + 8, 0x5a5a5a5a);
    sgi_migrate_stopped(from, uri);
    kona_command(to, 0x3e, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_BASE), ==, 0);
    g_assert_cmphex(guest_word(to, RETURN_BASE + 8), ==, 0x5a5a5a5a);
    x_initialise(to, 640);
    g_assert_cmphex(guest_word(to, RETURN_BASE), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_BASE + 8), ==, 640);
    kona_command(to, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_A), ==, 0);
    g_assert_cmphex(guest_word(to, RETURN_B), ==, 0);
    g_assert_cmpuint(kona_get_uint(to, "x-test-context-count"), ==, 2);
    context_restore_physical(to, SAVE_A);
    kona_command(to, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_A), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_A + 8), ==, 3);
    g_assert_cmphex(guest_word(to, RETURN_A + 12), ==, 5);
    g_assert_cmphex(guest_word(to, RETURN_A + 16), ==, 7);
    g_assert_cmphex(guest_word(to, RETURN_A + 20), ==, 11);
    context_restore_physical(to, SAVE_A + 32);
    kona_command(to, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_B), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_B + 8), ==, 13);
    g_assert_cmphex(guest_word(to, RETURN_B + 12), ==, 17);
    g_assert_cmphex(guest_word(to, RETURN_B + 16), ==, 19);
    g_assert_cmphex(guest_word(to, RETURN_B + 20), ==, 23);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_migration_pending_page(void)
{
    g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    QTestState *from = qtest_init(MIGRATION_ARGS);
    QTestState *to = qtest_initf(MIGRATION_ARGS " -incoming %s", uri);

    kona_command_kicked(from, 0x1fd,
        (uint32_t[]){ physical_page_word(SAVE_A), 0x9000, 0x10000 }, 3);
    sgi_migrate_stopped(from, uri);
    kona_command(to, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_A), ==, 0);
    kona_register_return_page(to, RETURN_A);
    kona_command(to, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(to, RETURN_A), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_A + 8), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_migration_replaced_retrace(void)
{
    g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    QTestState *from = qtest_init(MIGRATION_ARGS);
    QTestState *to = qtest_initf(MIGRATION_ARGS " -incoming %s", uri);

    context_first_physical(from, SAVE_A, RETURN_A);
    context_window(from, 20, 30, true);
    kona_command_kicked(from, 0x1e7, (uint32_t[]){ 7 }, 1);
    context_first_physical(from, SAVE_A, RETURN_B);
    context_window(from, 40, 50, true);
    sgi_migrate_stopped(from, uri);
    g_assert_cmphex(shown_pixel(to, 40, 50), ==, 0);
    qtest_clock_step(to, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(to, REG_RETRACE_TAG), ==, 7);
    g_assert_cmphex(shown_pixel(to, 40, 50), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_context_allocation_failure(void)
{
    for (unsigned replacement = 0; replacement < 2; replacement++) {
        QTestState *s = qtest_init(MACHINE_ARGS);
        g_autofree char *before = NULL, *after = NULL, *error = NULL;

        context_first_physical(s, SAVE_A, RETURN_A);
        kona_command(s, 0xe3, (uint32_t[]){ 3, 5, 7, 11 }, 4);
        kona_command(s, 0xc9, (uint32_t[]){ 0x800 }, 1);
        x_initialise(s, 640);
        guest_fill_word(s, RETURN_A, 0);
        before = kona_get_string(s, "x-test-context-digest");
        qtest_qom_set_bool(s, "/machine/module0/kona0",
                          "x-test-context-alloc-fail", true);
        context_first_physical(s, replacement ? SAVE_A : SAVE_B, RETURN_B);
        x_initialise(s, 320);
        assert_context_stop(s);
        after = kona_get_string(s, "x-test-context-digest");
        g_assert_cmpstr(after, ==, before);
        error = kona_get_string(s, "x-test-context-error");
        g_assert_nonnull(strstr(error, "state allocation failed"));
        g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 1);
        g_assert_cmpuint(kona_get_uint(s, "x-test-context-current"), ==, 0);
        g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
        g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 640);
        g_assert_cmphex(guest_word(s, RETURN_B), ==, 0);
        qtest_quit(s);
    }
}

static void test_context_reinitialization(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    context_first_physical(s, SAVE_A, RETURN_A);
    kona_command(s, 0xe3, (uint32_t[]){ 3, 5, 7, 11 }, 4);
    kona_command(s, 0xc9, (uint32_t[]){ 0x800 }, 1);
    guest_fill_word(s, RETURN_A, 0);
    kona_command_kicked(s, 0x1fd,
        (uint32_t[]){ physical_page_word(SAVE_A), 0x9000, 0x10000 }, 3);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 0);
    kona_register_return_page(s, RETURN_B);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 1);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(guest_word(s, RETURN_B + 8 + 4 * i), ==, 0);
    }
    kona_command(s, 0x4c, (uint32_t[]){ 0xbb0 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_B + 8), ==, 0);
    kona_command(s, 0xe3, (uint32_t[]){ 101, 103, 107, 109 }, 4);
    context_deactivate(s, 1);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_B + 8), ==, 101);
    g_assert_cmphex(guest_word(s, RETURN_B + 12), ==, 103);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    qtest_quit(s);
}

/* Failed page translation cannot resurrect an older generation's page. */
static void test_failed_context_registration(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    context_first_physical(s, SAVE_A, RETURN_A);
    kona_command_kicked(s, 0x1fd,
        (uint32_t[]){ physical_page_word(SAVE_A), 0x9000, 0x10000 }, 3);
    kona_command_kicked(s, 0x1f2,
        (uint32_t[]){ 0x14, 0x9000, 0x4000 }, 3);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    context_deactivate(s, 1);
    kona_register_return_page(s, RETURN_B);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 0);
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 1);
    qtest_quit(s);
}

static void test_reset_clears_pending_context(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    context_first_physical(s, SAVE_A, RETURN_A);
    context_window(s, 20, 30, true);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 7 }, 1);
    context_deactivate(s, 1);
    qtest_system_reset(s);
    guest_fill_word(s, RETURN_A, 0);
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 0);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    g_assert_cmphex(shown_pixel(s, 20, 30), ==, 0);
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 0);
    qtest_quit(s);
}

/* Current-window retirement retains both current and noncurrent records. */
static void test_context_retirement_retains_records(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    kona_command(s, 0xe3, (uint32_t[]){ 3, 5, 7, 11 }, 4);
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_B, RETURN_B);
    kona_command(s, 0xe3, (uint32_t[]){ 13, 17, 19, 23 }, 4);
    kona_command_kicked(s, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x103);
    context_deactivate(s, 2);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 3);
    context_deactivate(s, 1);
    context_restore_physical(s, SAVE_B);
    kona_command(s, 0x4c, (uint32_t[]){ 0xba2 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_B + 8), ==, 13);
    g_assert_cmpuint(kona_get_uint(s, "x-test-context-count"), ==, 2);
    qtest_quit(s);
}

static uint8_t *stream_bytes(uint8_t *data, size_t length,
                              const void *needle, size_t count)
{
    for (size_t i = 0; i + count <= length; i++) {
        if (!memcmp(data + i, needle, count)) {
            return data + i;
        }
    }
    g_assert_not_reached();
}

/*
 * An open X write's owner must be a context in use with its own
 * generation: an owner slot no context holds, and a generation that is
 * not the owner's, are refused on load.
 */
static void test_migration_rejects_x_pixel_owner(void)
{
    static const char owner[] = "sgi-kona/x-pixel-owner";

    for (unsigned kind = 0; kind < 2; kind++) {
        g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "stream", NULL);
        g_autofree char *uri = g_strdup_printf("file:%s", path);
        g_autofree char *data = NULL;
        QTestState *from = qtest_init(MIGRATION_ARGS);
        QTestState *to;
        uint8_t *fields;
        gsize length;

        context_first_physical(from, SAVE_A, RETURN_A);
        qtest_writel(from, REG_KICK, 0);
        x_write_start(from);
        sgi_migrate_stopped(from, uri);
        qtest_quit(from);
        g_assert_true(g_file_get_contents(path, &data, &length, NULL));
        /* The subsection's version, then the owner and its generation. */
        fields = stream_bytes((uint8_t *)data, length, owner,
                              sizeof(owner) - 1) + sizeof(owner) - 1 + 4;
        g_assert_cmpuint(ldl_be_p(fields), ==, 0);
        if (kind == 0) {
            stl_be_p(fields, 40);
        } else {
            stq_be_p(fields + 4, ldq_be_p(fields + 4) + 1);
        }
        g_assert_true(g_file_set_contents(path, data, length, NULL));
        to = qtest_init(MIGRATION_ARGS " -incoming defer");
        qtest_set_expected_status(to, 1);
        qtest_qmp_assert_success(to,
            "{ 'execute': 'migrate-incoming', 'arguments': { 'uri': %s } }",
            uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        g_unlink(path);
        g_rmdir(dir);
    }
}

/*
 * A saved GL data run the live input could not have left is refused on
 * load: a token that has no data run, a run with no words to come, and a
 * texel load holding more words than its rows leave room for. The source
 * saves A's texel load, cut after two of its four words, when B sends.
 */
static void test_migration_rejects_saved_gl_run(void)
{
    static const char run[] = "sgi-kona/saved-input-entry/gl-run";

    for (unsigned kind = 0; kind < 3; kind++) {
        g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "stream", NULL);
        g_autofree char *uri = g_strdup_printf("file:%s", path);
        g_autofree char *data = NULL;
        QTestState *from = qtest_init(MIGRATION_ARGS);
        QTestState *to;
        uint8_t *fields;
        gsize length;

        context_first_physical(from, SAVE_A, RETURN_A);
        qtest_writel(from, REG_KICK, 0);
        gl_run_texture_start(from);
        context_deactivate(from, 1);
        context_first_physical(from, SAVE_B, RETURN_B);
        qtest_writel(from, REG_KICK, 0);
        kona_command(from, 0x177, (uint32_t[]){ 0x77000000 }, 1);
        sgi_migrate_stopped(from, uri);
        qtest_quit(from);
        g_assert_true(g_file_get_contents(path, &data, &length, NULL));
        /* The subsection's version, then token, remaining and held words. */
        fields = stream_bytes((uint8_t *)data, length, run, sizeof(run) - 1) +
                 sizeof(run) - 1 + 4;
        g_assert_cmphex(ldl_be_p(fields), ==, 0x174);
        g_assert_cmpuint(ldl_be_p(fields + 4), ==, 2);
        g_assert_cmpuint(ldl_be_p(fields + 8), ==, 2);
        if (kind == 0) {
            stl_be_p(fields, 0x177);
        } else if (kind == 1) {
            stl_be_p(fields + 4, 0);
        } else {
            stl_be_p(fields + 8, 3);
        }
        g_assert_true(g_file_set_contents(path, data, length, NULL));
        to = qtest_init(MIGRATION_ARGS " -incoming defer");
        qtest_set_expected_status(to, 1);
        qtest_qmp_assert_success(to,
            "{ 'execute': 'migrate-incoming', 'arguments': { 'uri': %s } }",
            uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        g_unlink(path);
        g_rmdir(dir);
    }
}

/*
 * A textport bitmap the live framer could not have left is refused on
 * load: a header with more than its six words held, whose next word would
 * be written past the header, and a payload count past the model's bound,
 * which the load would otherwise try to allocate. The source is left
 * collecting the header, two words in, whose distinctive values find the
 * counts in the stream.
 */
static void test_migration_rejects_bitmap_stream(void)
{
    static const uint8_t header[] = {
        0x13, 0x57, 0x24, 0x68, 0x24, 0x68, 0x13, 0x57,
    };

    for (unsigned kind = 0; kind < 2; kind++) {
        g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "stream", NULL);
        g_autofree char *uri = g_strdup_printf("file:%s", path);
        g_autofree char *data = NULL;
        QTestState *from = qtest_init(MIGRATION_ARGS);
        QTestState *to;
        uint8_t *fields;
        gsize length;

        textport_command(from, 0xa, (uint32_t[]){ 0x13572468, 0x24681357 },
                         2);
        sgi_migrate_stopped(from, uri);
        qtest_quit(from);
        g_assert_true(g_file_get_contents(path, &data, &length, NULL));
        /* The header's six words, then held, width, height, have, expected. */
        fields = stream_bytes((uint8_t *)data, length, header, sizeof(header));
        g_assert_cmpuint(ldl_be_p(fields - 4), ==, 1);
        g_assert_cmpuint(ldl_be_p(fields + 24), ==, 2);
        if (kind == 0) {
            stl_be_p(fields + 24, 100);
        } else {
            stl_be_p(fields + 24 + 16, 0x40000000);
        }
        g_assert_true(g_file_set_contents(path, data, length, NULL));
        to = qtest_init(MIGRATION_ARGS " -incoming defer");
        qtest_set_expected_status(to, 1);
        qtest_qmp_assert_success(to,
            "{ 'execute': 'migrate-incoming', 'arguments': { 'uri': %s } }",
            uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        g_unlink(path);
        g_rmdir(dir);
    }
}

/* Corrupt the actual same-build stream, not a mocked load path. */
static void corrupt_context_stream(const char *path, unsigned kind)
{
    static const char contexts[] = "sgi-kona/contexts";
    static const char retrace[] = "sgi-kona/retrace";
    g_autofree char *data = NULL;
    gsize length;
    uint8_t *name, *records;

    g_assert_true(g_file_get_contents(path, &data, &length, NULL));
    name = stream_bytes((uint8_t *)data, length,
                        contexts, sizeof(contexts) - 1);
    g_assert_cmpuint(name[-2], ==, 5); /* QEMU_VM_SUBSECTION */
    g_assert_cmpuint(ldl_be_p(name + sizeof(contexts) - 1), ==, 7);
    /* Version, partial-table count and generation clock precede the records. */
    records = name + sizeof(contexts) - 1 + 4 + 4 + 8;
    g_assert_cmpuint(ldl_be_p(records), ==, kind == 5 ? 0 : 2);
    switch (kind) {
    case 0:
        stl_be_p(records, 65);
        break;
    case 1: {
        uint8_t second[26] = { 1 };
        uint8_t *record;

        stl_be_p(second + 1, physical_page_word(SAVE_B));
        second[9] = 1;
        stq_be_p(second + 10, RETURN_B);
        stq_be_p(second + 18, 2);
        record = stream_bytes(records + 5,
            length - (records + 5 - (uint8_t *)data), second, sizeof(second));
        stl_be_p(record + 1, physical_page_word(SAVE_A));
        break;
    }
    case 2:
        stq_be_p(records + 5 + 18, UINT64_MAX);
        break;
    case 3:
        name = stream_bytes((uint8_t *)data, length,
                            retrace, sizeof(retrace) - 1);
        /* Version, tag, valid flag and sequence precede the generation. */
        stq_be_p(name + sizeof(retrace) - 1 + 4 + 4 + 1 + 8, UINT64_MAX);
        break;
    case 5:
        g_assert_cmpuint(records[4], ==, 1);
        g_assert_cmpuint(records[5], ==, 1);
        g_assert_cmpuint(records[5 + 9], ==, 0);
        /* Pipe-only registration cannot give the transient record a page. */
        records[5 + 9] = 1;
        stq_be_p(records + 5 + 10, RETURN_A);
        break;
    default:
        g_assert_not_reached();
    }
    g_assert_true(g_file_set_contents(path, data, length, NULL));
}

/*
 * The saved-input subsection's first entry and its transient entry, as
 * vmstate_sgi_kona_saved_one lays an entry out: valid; the textport and
 * GL framers (framing, selector, expected and held counts, 32 words);
 * the sync, video, colour map, gamma, cursor and texture-table fields;
 * the list invocation's token, need, have and 16 words; the stipple
 * store; the kernel framer's user command; the textport bitmap's state,
 * six header words, five counts and payload, none while no bitmap is open.
 */
#define SAVED_ENTRY_BYTES   1812
#define SAVED_GL_PORT       142
#define SAVED_CALLS_TOKEN   1531
#define SAVED_KERNEL_PORT   1623
#define SAVED_BITMAP        1764
#define SAVED_TRANSIENT     64

/* A saved framer's selector, expected and held counts. */
static void assert_saved_port(const uint8_t *port, uint32_t selector,
                              uint32_t expected, uint32_t held)
{
    g_assert_cmpuint(port[0], ==, 1);
    g_assert_cmphex(ldl_be_p(port + 1), ==, selector);
    g_assert_cmpuint(ldl_be_p(port + 5), ==, expected);
    g_assert_cmpuint(ldl_be_p(port + 9), ==, held);
}

static void corrupt_saved_input_stream(const char *path, unsigned kind)
{
    static const char saved[] = "sgi-kona/saved-input";
    static const char bracket[] = "sgi-kona/bracket";
    g_autofree char *data = NULL;
    gsize length;
    uint8_t *name, *entry, *port;

    g_assert_true(g_file_get_contents(path, &data, &length, NULL));
    if (kind >= 9) {
        /* Version, kernel_input, suspended_owner and the privileged bit. */
        name = stream_bytes((uint8_t *)data, length, bracket,
                            sizeof(bracket) - 1);
        g_assert_cmpuint(name[-2], ==, 5); /* QEMU_VM_SUBSECTION */
        g_assert_cmpuint(ldl_be_p(name + sizeof(bracket) - 1), ==, 2);
        port = name + sizeof(bracket) - 1 + 4 + 1 + 4 + 1;
        /* Context A's list call, set aside: 0x16d, two expected, one held. */
        assert_saved_port(port, 0x16d, 2, 1);
        if (kind == 9) { /* every word held */
            stl_be_p(port + 9, 2);
        } else { /* set aside with no suspended owner */
            stl_be_p(port - 5, (uint32_t)-1);
        }
        g_assert_true(g_file_set_contents(path, data, length, NULL));
        return;
    }
    name = stream_bytes((uint8_t *)data, length, saved, sizeof(saved) - 1);
    g_assert_cmpuint(name[-2], ==, 5); /* QEMU_VM_SUBSECTION */
    g_assert_cmpuint(ldl_be_p(name + sizeof(saved) - 1), ==, 3);
    entry = name + sizeof(saved) - 1 + 4;
    g_assert_cmpuint(entry[0], ==, 1);
    if (kind < 7) {
        /* Context A's saved viewport: 0xe3, four expected, two held. */
        assert_saved_port(entry + SAVED_GL_PORT, 0xe3, 4, 2);
    } else {
        /* Context A's saved list call: 0x16d, two expected, one held. */
        assert_saved_port(entry + SAVED_KERNEL_PORT, 0x16d, 2, 1);
    }
    switch (kind) {
    case 0: /* every word held: the next one would land past the packet */
        stl_be_p(entry + SAVED_GL_PORT + 9, 4);
        break;
    case 1: /* a selector the GL table does not have */
        stl_be_p(entry + SAVED_GL_PORT + 1, 0xffff);
        break;
    case 2: /* a list invocation with an odd count, all of it held */
        stl_be_p(entry + SAVED_CALLS_TOKEN, 0x170);
        stl_be_p(entry + SAVED_CALLS_TOKEN + 4, 3);
        stl_be_p(entry + SAVED_CALLS_TOKEN + 8, 3);
        break;
    case 3: /* a copy for the transient slot, which holds no context */
        entry[SAVED_TRANSIENT * SAVED_ENTRY_BYTES] = 1;
        break;
    case 4: /* a bitmap collecting a payload no header sized */
        stl_be_p(entry + SAVED_BITMAP, 2);
        break;
    case 5: /* a bitmap header with all six words held */
        stl_be_p(entry + SAVED_BITMAP, 1);
        stl_be_p(entry + SAVED_BITMAP + 28, 6);
        break;
    case 6: /* an empty slot with a bitmap open */
        stl_be_p(entry + SAVED_ENTRY_BYTES + SAVED_BITMAP, 1);
        break;
    case 7: /* a kernel-framer command with every word held */
        stl_be_p(entry + SAVED_KERNEL_PORT + 9, 2);
        break;
    case 8: /* a selector the kernel framer's table does not have */
        stl_be_p(entry + SAVED_KERNEL_PORT + 1, 0xffff);
        break;
    default:
        g_assert_not_reached();
    }
    g_assert_true(g_file_set_contents(path, data, length, NULL));
}

/*
 * A saved command the live input could not have held is refused on load:
 * a GL packet with all its words held, an unknown GL selector, a list
 * invocation with an odd held count, a copy for an unused transient slot,
 * a bitmap payload no header sized, a bitmap header already complete, a
 * bitmap in an empty slot, a saved kernel-framer command with all its
 * words held or an unknown selector, and a set-aside one with all its
 * words held or no suspended owner. A destination that took them would
 * write past the framer's, the invocation's or the bitmap's words on the
 * next word.
 */
static void test_migration_rejects_saved_input(void)
{
    for (unsigned kind = 0; kind < 11; kind++) {
        g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "stream", NULL);
        g_autofree char *uri = g_strdup_printf("file:%s", path);
        QTestState *from = qtest_init(MIGRATION_ARGS);
        QTestState *to;

        context_first_physical(from, SAVE_A, RETURN_A);
        qtest_writel(from, REG_KICK, 0);
        if (kind < 7) {
            kona_command(from, 0xe3, (uint32_t[]){ 1, 9 }, 2);
        } else {
            kona_command(from, 0x16d, (uint32_t[]){ 0x6000 }, 1);
        }
        context_deactivate(from, 1);
        if (kind < 9) {
            context_first_physical(from, SAVE_B, RETURN_B);
            qtest_writel(from, REG_KICK, 0);
            kona_command(from, 0x177, (uint32_t[]){ 0x77000000 }, 1);
        }
        sgi_migrate_stopped(from, uri);
        qtest_quit(from);
        corrupt_saved_input_stream(path, kind);
        to = qtest_init(MIGRATION_ARGS " -incoming defer");
        qtest_set_expected_status(to, 1);
        qtest_qmp_assert_success(to,
            "{ 'execute': 'migrate-incoming', 'arguments': { 'uri': %s } }",
            uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        g_unlink(path);
        g_rmdir(dir);
    }
}

static void test_migration_rejects_context_stream(void)
{
    for (unsigned kind = 0; kind < 7; kind++) {
        g_autofree char *dir = g_dir_make_tmp("sn0-kona-context-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "stream", NULL);
        g_autofree char *uri = g_strdup_printf("file:%s", path);
        g_autofree char *log = NULL;
        g_autofree char *found = NULL;
        QTestState *from = qtest_init(MIGRATION_ARGS);
        QTestState *to;

        if (kind < 5) {
            context_first_physical(from, SAVE_A, RETURN_A);
            context_deactivate(from, 1);
            context_first_physical(from, SAVE_B, RETURN_B);
            kona_command_kicked(from, 0x1e7, (uint32_t[]){ 7 }, 1);
        } else {
            kona_command_kicked(from, 0x1f9,
                (uint32_t[]){ SAVE_MARKER, physical_page_word(SAVE_A),
                              0x9000, 0xedededed }, 4);
            kona_register_return_page(from, RETURN_A);
            if (kind == 6) {
                context_deactivate(from, 1);
            }
        }
        sgi_migrate_stopped(from, uri);
        qtest_quit(from);
        if (kind != 4) {
            corrupt_context_stream(path, kind < 4 ? kind : 5);
        }
        to = kona_init_logged_args(MIGRATION_ARGS " -incoming defer", &log);
        if (kind == 4) {
            qtest_qom_set_bool(to, "/machine/module0/kona0",
                              "x-test-context-alloc-fail", true);
        }
        qtest_set_expected_status(to, 1);
        qtest_qmp_assert_success(to,
            "{ 'execute': 'migrate-incoming', 'arguments': { 'uri': %s } }",
            uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        found = kona_log_read(log);
        g_assert_null(strstr(found, "sgi_kona_return_post "));
        g_assert_null(strstr(found, "sgi_kona_retrace_done "));
        g_unlink(path);
        g_rmdir(dir);
    }
}

/*
 * The 0xee answer with one context: its return page, filled with 0x5a,
 * gets 32 zero words at bytes 8 to 0x87 and then a non-zero word 0, with
 * word 1 and the bytes past 0x87 untouched. The trace shows the 32 words
 * written before word 0, so they are present when the client's wait ends.
 */
static void test_line_distance_answer(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *data = NULL, *ready = NULL;
    uint8_t words[0x80], zeros[0x80] = { 0 };
    const char *data_write, *ready_write;
    QTestState *s = kona_init_logged(&log);

    for (unsigned i = 0; i < 0x100; i += 4) {
        guest_fill_word(s, RETURN_A + i, 0x5a5a5a5a);
    }
    guest_fill_word(s, RETURN_A, 0);
    context_first_physical(s, SAVE_A, RETURN_A);
    kona_command(s, 0xee, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), !=, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 4), ==, 0x5a5a5a5a);
    qtest_memread(s, RETURN_A + 8, words, sizeof(words));
    g_assert_cmpmem(words, sizeof(words), zeros, sizeof(zeros));
    g_assert_cmphex(guest_word(s, RETURN_A + 0x88), ==, 0x5a5a5a5a);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    data = g_strdup_printf("addr 0x%" PRIx64 " len 0x80\n", RETURN_A + 8);
    ready = g_strdup_printf("addr 0x%" PRIx64 " len 0x4\n", RETURN_A);
    data_write = strstr(found, data);
    ready_write = strstr(found, ready);
    g_assert_nonnull(data_write);
    g_assert_nonnull(ready_write);
    g_assert_true(data_write < ready_write);
    g_assert_nonnull(strstr(found, "sgi_kona_return_post token 0xee "));
}

/*
 * With two contexts, the swap sequence from the traced run: A's first
 * activation and page, deactivate, B's, deactivate, A restored by 0x1f9
 * alone. The GL client's 0xee then lands its 32 words and word 0 on A's
 * page, and B's page is untouched.
 */
static void test_line_distance_on_current_context(void)
{
    uint8_t words[0x80], zeros[0x80] = { 0 };
    QTestState *s = qtest_init(MACHINE_ARGS);

    for (unsigned i = 0; i < 0x100; i += 4) {
        guest_fill_word(s, RETURN_A + i, 0x5a5a5a5a);
        guest_fill_word(s, RETURN_B + i, 0x5a5a5a5a);
    }
    guest_fill_word(s, RETURN_A, 0);
    guest_fill_word(s, RETURN_B, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_first_physical(s, SAVE_A, RETURN_A);
    context_deactivate(s, 1);
    context_first_physical(s, SAVE_B, RETURN_B);
    context_deactivate(s, 2);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0xee, (uint32_t[]){ 0 }, 1);

    g_assert_cmphex(guest_word(s, RETURN_A), !=, 0);
    qtest_memread(s, RETURN_A + 8, words, sizeof(words));
    g_assert_cmpmem(words, sizeof(words), zeros, sizeof(zeros));
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_B + 8), ==, 0x5a5a5a5a);
    qtest_quit(s);
}

/* A context A whose return page holds 0x5a in its first 0x100 bytes. */
static void context_a_with_filled_page(QTestState *s)
{
    for (unsigned i = 0; i < 0x100; i += 4) {
        guest_fill_word(s, RETURN_A + i, 0x5a5a5a5a);
    }
    guest_fill_word(s, RETURN_A, 0);
    context_first_physical(s, SAVE_A, RETURN_A);
}

/*
 * glGetError's 0x3e: the error code 0 at byte 8 of the current context's
 * return page, then a non-zero word 0, with words 1 and 3 untouched and
 * the code written first. The library waits for word 0 without limit, so
 * a model that drops 0x3e hangs every drawing case.
 */
static void test_get_error_answer(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *data = NULL, *ready = NULL;
    const char *data_write, *ready_write;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), !=, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 4), ==, 0x5a5a5a5a);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 12), ==, 0x5a5a5a5a);
    qtest_quit(s);

    found = kona_log_read(log);
    data = g_strdup_printf("addr 0x%" PRIx64 " len 0x4\n", RETURN_A + 8);
    ready = g_strdup_printf("addr 0x%" PRIx64 " len 0x4\n", RETURN_A);
    data_write = strstr(found, data);
    ready_write = strstr(found, ready);
    g_assert_nonnull(data_write);
    g_assert_nonnull(ready_write);
    g_assert_true(data_write < ready_write);
    g_assert_nonnull(strstr(found, "sgi_kona_return_post token 0x3e "));
}

static uint32_t fbits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/* Words from byte 8 of A's return page, after the reply's word 0. */
static void assert_return_floats(QTestState *s, uint32_t offset,
                                 const float *v, unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        g_assert_cmphex(guest_word(s, RETURN_A + 8 + offset + 4 * k), ==,
                        fbits(v[k]));
    }
}

/*
 * The raster-position query 0x3d takes one word, the 0 the library
 * sends, and answers fifteen binary32 words from byte 8: raster RGBA,
 * index, texture STRQ, window x, y, z and clip w, valid as 1.0 or 0.0,
 * and the eye distance. Under a 100 by 100 viewport and identity
 * matrices, glRasterPos2f(0, 0) lands at the window centre at depth
 * 0.5; (5, 5) lies outside the view volume, so the position is invalid.
 * A model with no 0x3d drops both words as textport words and never
 * answers, and gr_osview's glGet of GL_CURRENT_RASTER_POSITION hangs.
 */
static void test_raster_query_answer(void)
{
    static const float colour[4] = { 0.25f, 0.5f, 0.75f, 1 };
    static const float index[1] = { 1 };
    static const float strq[4] = { 0, 0, 0, 1 };
    static const float centre[4] = { 50, 50, 0.5f, 1 };
    static const float valid[1] = { 1 }, invalid[1] = { 0 };
    static const float distance[1] = { 0 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    kona_command(s, 0xe3, (uint32_t[]){ 0, 0, 100, 100 }, 4);
    kona_command(s, 0x63, (uint32_t[]){ fbits(colour[0]), fbits(colour[1]),
                                        fbits(colour[2]) }, 3);
    kona_command(s, 0xa9, (uint32_t[]){ fbits(0), fbits(0) }, 2);
    qtest_writel(s, REG_COMMAND, 0x3d);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    qtest_writel(s, REG_COMMAND, 0);
    g_assert_cmphex(guest_word(s, RETURN_A), !=, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 4), ==, 0x5a5a5a5a);
    assert_return_floats(s, 0x00, colour, 4);
    assert_return_floats(s, 0x10, index, 1);
    assert_return_floats(s, 0x14, strq, 4);
    assert_return_floats(s, 0x24, centre, 4);
    assert_return_floats(s, 0x34, valid, 1);
    assert_return_floats(s, 0x38, distance, 1);
    g_assert_cmphex(guest_word(s, RETURN_A + 8 + 0x3c), ==, 0x5a5a5a5a);

    guest_fill_word(s, RETURN_A, 0);
    kona_command(s, 0xa9, (uint32_t[]){ fbits(5), fbits(5) }, 2);
    kona_command(s, 0x3d, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), !=, 0);
    assert_return_floats(s, 0x34, invalid, 1);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_cmpuint(count_occurrences(found,
                                       "sgi_kona_return_post token 0x3d "),
                     ==, 2);
}

/*
 * The current-attribute query 0x3b takes one word and answers thirteen
 * binary32 words from byte 8: the current RGBA glColor3f set, the index,
 * the normal, texture STRQ, and a word for pname 0xb43, whose meaning is
 * unknown and which the model answers 0. libGLcore sends glGetFloatv of
 * GL_CURRENT_COLOR this way, not as the 0x4c glGet.
 */
static void test_current_query_answer(void)
{
    static const float colour[4] = { 0.25f, 0.5f, 0.75f, 1 };
    static const float index[1] = { 1 };
    static const float normal[3] = { 0, 0, 1 };
    static const float strq[4] = { 0, 0, 0, 1 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    kona_command(s, 0x63, (uint32_t[]){ fbits(colour[0]), fbits(colour[1]),
                                        fbits(colour[2]) }, 3);
    qtest_writel(s, REG_COMMAND, 0x3b);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    qtest_writel(s, REG_COMMAND, 0);
    g_assert_cmphex(guest_word(s, RETURN_A), !=, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 4), ==, 0x5a5a5a5a);
    assert_return_floats(s, 0x00, colour, 4);
    assert_return_floats(s, 0x10, index, 1);
    assert_return_floats(s, 0x14, normal, 3);
    assert_return_floats(s, 0x20, strq, 4);
    g_assert_cmphex(guest_word(s, RETURN_A + 8 + 0x30), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 8 + 0x34), ==, 0x5a5a5a5a);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "sgi_kona_return_post token 0x3b "));
}

/* An outgoing page does not authorize a context-owned error reply. */
static void test_reply_after_deactivation(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    context_a_with_filled_page(s);
    kona_command(s, 0xe4, (uint32_t[]){ 0x501 }, 1);
    context_deactivate(s, 1);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 0x5a5a5a5a);
    g_assert_cmphex(guest_word(s, RETURN_A + 4), ==, 0x5a5a5a5a);

    context_first_physical(s, SAVE_B, RETURN_B);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    g_assert_cmphex(guest_word(s, RETURN_B), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_B + 8), ==, 0);
    context_deactivate(s, 1);
    context_restore_physical(s, SAVE_A);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 0x501);
    guest_fill_word(s, RETURN_A, 0);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 1);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 0);
    qtest_quit(s);
}

/* glFinish's 0xc6 1: a non-zero word 0 and nothing else on the page. */
static void test_finish_sets_word_0(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    context_a_with_filled_page(s);
    kona_command(s, 0xc6, (uint32_t[]){ 1 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), !=, 0);
    for (unsigned i = 4; i < 0x100; i += 4) {
        g_assert_cmphex(guest_word(s, RETURN_A + i), ==, 0x5a5a5a5a);
    }
    assert_textport_in_step(s);
    qtest_quit(s);
}

/*
 * A buffer swap's 0xc6 0 frames as one command and leaves the page
 * untouched: a word 0 posted here would end the next glGetError's wait
 * before its answer. A model that answers every 0xc6 fails the page
 * check; one that drops 0xc6 fails the framing check.
 */
static void test_swap_finish_posts_nothing(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    kona_command(s, 0xc6, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), ==, 0);
    for (unsigned i = 4; i < 0x100; i += 4) {
        g_assert_cmphex(guest_word(s, RETURN_A + i), ==, 0x5a5a5a5a);
    }
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0xc6 args 0x0 0x0 0x0 argc 1\n"));
    g_assert_null(strstr(found, "sgi_kona_return_post "));
}

/*
 * The probe's clear case: viewport 0 0 128 128,
 * disable dither, clear colour (1, 0, 0, 1), clear the colour buffer,
 * flush, then glGetError. Each frames whole with nothing dropped,
 * glGetError answers on the context's page, and a reason post sent after
 * it still frames and posts.
 */
static void test_clear_stream_framed(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    guest_fill_word(s, REASON_PAGE, 0);
    kona_register_reason_page(s, REASON_PAGE);
    context_a_with_filled_page(s);
    kona_command(s, 0xe3, (uint32_t[]){ 0, 0, 0x80, 0x80 }, 4);
    kona_command(s, 0x25, (uint32_t[]){ 0xbd0 }, 1);
    kona_command(s, 0x1d,
                 (uint32_t[]){ 0x3f800000, 0, 0, 0x3f800000 }, 4);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    kona_command(s, 0xc7, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    g_assert_cmphex(guest_word(s, RETURN_A), !=, 0);
    g_assert_cmphex(guest_word(s, RETURN_A + 8), ==, 0);
    kona_command_kicked(s, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(guest_word(s, REASON_PAGE), ==, 0x103);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0xe3 args 0x0 0x0 0x80 argc 4\n"));
    g_assert_nonnull(strstr(found, "token 0x25 args 0xbd0 0x0 0x0 argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x1d args 0x3f800000 0x0 0x0"
                                   " argc 4\n"));
    g_assert_nonnull(strstr(found, "token 0x1a args 0x4000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0xc7 args 0x0 0x0 0x0 argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x3e args 0xdbdbdbdb 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x1f8 args 0x103 0x0 0x0"
                                   " argc 1\n"));
}

/*
 * The triangle case's words, with arguments shaped to misframe on a
 * wrong count: shade model, colour, every begin from 0x57 to 0x60, three
 * vertices, end, finish and the kernel's retrace request. Each frames
 * whole, nothing is dropped, and the textport answers its sync probe
 * afterwards.
 */
static void test_triangle_stream_framed(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    kona_command(s, 0xc0, (uint32_t[]){ 0x1d00 }, 1);
    kona_command(s, 0x63, (uint32_t[]){ 1, 9, 0x3f800000 }, 3);
    for (uint32_t token = 0x57; token <= 0x60; token++) {
        kona_command(s, token, (uint32_t[]){ 0x5b000000 | token }, 1);
    }
    kona_command(s, 0x8c, (uint32_t[]){ 1, 9 }, 2);
    kona_command(s, 0x8c, (uint32_t[]){ 0x3f000000, 0xbf000000 }, 2);
    kona_command(s, 0x8c, (uint32_t[]){ 0, 0x3f000000 }, 2);
    kona_command(s, 0x71, (uint32_t[]){ 0x71000000 }, 1);
    kona_command(s, 0xc6, (uint32_t[]){ 1 }, 1);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 0xe7000000 }, 1);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "token 0xc0 args 0x1d00 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x63 args 0x1 0x9 0x3f800000"
                                   " argc 3\n"));
    for (uint32_t token = 0x57; token <= 0x60; token++) {
        g_autofree char *line = g_strdup_printf(
            "token 0x%x args 0x%x 0x0 0x0 argc 1\n", token,
            0x5b000000 | token);

        g_assert_nonnull(strstr(found, line));
    }
    g_assert_nonnull(strstr(found, "token 0x8c args 0x1 0x9 0x0 argc 2\n"));
    g_assert_nonnull(strstr(found, "token 0x8c args 0x0 0x3f000000 0x0"
                                   " argc 2\n"));
    g_assert_nonnull(strstr(found, "token 0x71 args 0x71000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0xc6 args 0x1 0x0 0x0 argc 1\n"));
    g_assert_nonnull(strstr(found, "token 0x1e7 args 0xe7000000 0x0 0x0"
                                   " argc 1\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_kernel_command "),
                     ==, 2 + 2 + 10 + 3 + 3);
}

/*
 * The GL client's buffer configuration and the textport's sync word
 * share the word 1. Before any context is activated the page speaks the
 * textport grammar: the configuration's first words, 1 0, are the sync
 * word and a selector the textport lacks, and 1 9 0 is the sync probe.
 * After an activation the configuration as the traced make-current sent
 * it frames whole: 1 and ten words for a single-buffered visual with
 * 12-bit components, 2 0, 0x114 and its word, and 4 and ten words, with
 * no word dropped and none of the zeros and 0xc framed as a scissor. The
 * next token frames, and the textport still answers its sync probe,
 * since a 1 followed by a textport command is the sync word in either
 * grammar.
 */
static void test_buffer_config_after_activation(void)
{
    static const uint32_t config[10] = { 0, 0xc, 0, 0, 0, 0, 0, 0, 0, 0 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *activated;
    QTestState *s = kona_init_logged(&log);

    kona_command(s, 1, (uint32_t[]){ 0, 9, 0 }, 3);
    assert_textport_in_step(s);
    context_a_with_filled_page(s);
    kona_command(s, 1, config, 10);
    kona_command(s, 2, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x114, (uint32_t[]){ 3 }, 1);
    kona_command(s, 4, config, 10);
    kona_command(s, 0x177, (uint32_t[]){ 0x77000000 }, 1);
    assert_textport_in_step(s);
    g_assert_cmphex(qtest_readl(s, KONA_REG(0x800208)), ==, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    activated = strstr(found, "sgi_kona_context_activate token 0x1fd");
    g_assert_nonnull(activated);
    /* Before it: the textport grammar, as before the GL grammar existed. */
    g_assert_nonnull(strstr(found, "unsupported textport selector 0x0"));
    g_assert_true(strstr(found, "unsupported textport selector 0x0") <
                  activated);
    g_assert_false(log_shows_misframing(activated));
    g_assert_nonnull(strstr(activated, "sgi_kona_gl_buffer_config token 0x1"
                                       " level 0 rgb 12 index 0 double 0\n"));
    g_assert_nonnull(strstr(activated, "token 0x2 argc 1 consumed"));
    g_assert_nonnull(strstr(activated, "token 0x114 args 0x3 0x0 0x0"
                                       " argc 1\n"));
    g_assert_nonnull(strstr(activated, "unknown read binding;"
                                       " reads refused for this context"));
    g_assert_nonnull(strstr(activated, "sgi_kona_gl_buffer_config token 0x4"
                                       " level 0 rgb 12 index 0 double 0\n"));
    g_assert_nonnull(strstr(activated, "token 0x4 args 0x0 0xc 0x0"
                                       " argc 10\n"));
    g_assert_null(strstr(activated, "token 0xc args"));
    g_assert_nonnull(strstr(activated, "token 0x177 args 0x77000000 0x0 0x0"
                                       " argc 1\n"));
}

/*
 * 0x1e0 takes two words. A traced ivview start sent nine of them after
 * its scissor, then glEnable of the depth test, 0x24 0xb71; the pairs
 * below are those the guest sent. With no length for 0x1e0 the words
 * arrive one by one between packets: the 1 among them opens a textport
 * packet that takes the following words, and the zeros and 0x1e0 are
 * dropped as neither a token nor a sync word. Framed, nothing is dropped
 * and the enable after them frames whole.
 */
static void test_tex_table_usage_framed(void)
{
    static const uint32_t pairs[9][2] = {
        { 0, 0 }, { 2, 0 }, { 1, 0 }, { 0, 1 }, { 2, 1 },
        { 1, 1 }, { 0, 2 }, { 2, 2 }, { 1, 2 },
    };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *activated;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    kona_command(s, 0xc, (uint32_t[]){ 0, 0, 0x1cc, 0x1c2 }, 4);
    for (unsigned i = 0; i < ARRAY_SIZE(pairs); i++) {
        kona_command(s, 0x1e0, pairs[i], 2);
    }
    kona_command(s, 0x24, (uint32_t[]){ 0xb71 }, 1);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    activated = strstr(found, "sgi_kona_context_activate token 0x1fd");
    g_assert_nonnull(activated);
    g_assert_false(log_shows_misframing(activated));
    g_assert_cmpuint(count_occurrences(activated,
                                       "token 0x1e0 argc 2 consumed"),
                     ==, ARRAY_SIZE(pairs));
    g_assert_nonnull(strstr(activated, "token 0x1e0 args 0x1 0x2 0x0"
                                       " argc 2\n"));
    g_assert_nonnull(strstr(activated, "token 0x24 args 0xb71 0x0 0x0"
                                       " argc 1\n"));
}

/*
 * IRIS GL's IGLOO library calls glIglooInterfaceSGIX, which libGLcore
 * sends as one selector and one word: 0x112 IRIS GL mode, 0x113 LMC
 * colour, 0x1be colour-normal, 0x1c9 tmesh mode and 0x1a9 swap-tmesh.
 * The first seven commands below are those an IRIS GL atlantis sent after
 * its make-current; 0x1a9 and 0x1c9 come from the same sender. Unframed,
 * 0x112 is dropped, its 1 opens a ten-word buffer configuration that
 * swallows the commands after it, and the client's later query is lost.
 * Framed, nothing is dropped, no buffer configuration is seen and the
 * colour, index and enable after them frame whole. The model keeps
 * colour-normal and LMC colour for IRIS GL's colour and normal rule and
 * consumes the other three.
 */
static void test_igloo_interface_framed(void)
{
    static const struct {
        uint32_t token;
        uint32_t word;
    } sent[] = {
        { 0x24, 0xba1 }, { 0xce, 0x203 }, { 0x112, 1 }, { 0x113, 1 },
        { 0x1be, 0 }, { 0xc7, 0 }, { 0x113, 0 }, { 0x1a9, 0 }, { 0x1c9, 1 },
    };
    static const uint32_t igloo[] = { 0x112, 0x1a9, 0x1c9 };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *activated;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    for (unsigned i = 0; i < ARRAY_SIZE(sent); i++) {
        kona_command(s, sent[i].token, &sent[i].word, 1);
    }
    kona_command(s, 0x6b, (uint32_t[]){ 0, 0, 0, 0x3f800000 }, 4);
    kona_command(s, 0x73, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x24, (uint32_t[]){ 0xb71 }, 1);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    activated = strstr(found, "sgi_kona_context_activate token 0x1fd");
    g_assert_nonnull(activated);
    g_assert_false(log_shows_misframing(activated));
    g_assert_null(strstr(activated, "sgi_kona_gl_buffer_config"));
    for (unsigned i = 0; i < ARRAY_SIZE(igloo); i++) {
        g_autofree char *line =
            g_strdup_printf("token 0x%x argc 1 consumed", igloo[i]);

        g_assert_cmpuint(count_occurrences(activated, line), ==, 1);
    }
    g_assert_cmpuint(count_occurrences(activated, "sgi_kona_gl_iris_rule"),
                     ==, 3);
    g_assert_nonnull(strstr(activated, "colour-normal 0x0 lmc colour 0x0\n"));
    g_assert_nonnull(strstr(activated, "token 0x112 args 0x1 0x0 0x0"
                                       " argc 1\n"));
    g_assert_nonnull(strstr(activated, "token 0x6b args 0x0 0x0 0x0"
                                       " argc 4\n"));
    g_assert_nonnull(strstr(activated, "token 0x73 args 0x0 0x0 0x0"
                                       " argc 1\n"));
    g_assert_nonnull(strstr(activated, "token 0x24 args 0xb71 0x0 0x0"
                                       " argc 1\n"));
}

/*
 * glTexGen is 0x14 and nine words: coordinate, parameter name and seven
 * value words. The four packets are those an IRIS GL atlantis sent while
 * setting up its texture: an eye-linear mode for S and T, then each eye
 * plane. Unframed, each word is dropped as neither a token nor a sync
 * word, and a plane's float can land on a selector. Framed, nothing is
 * dropped, the enable after them frames whole, and the shared GL takes
 * all four rather than the model consuming them unmodelled.
 */
static void test_tex_gen_framed(void)
{
    static const uint32_t sent[4][9] = {
        { 0x2000, 0x2500, 0x2400 },
        { 0x2000, 0x2502, 0x3a03126f },
        { 0x2001, 0x2500, 0x2400 },
        { 0x2001, 0x2502, 0, 0, 0x3a03126f },
    };
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    const char *activated;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    for (unsigned i = 0; i < ARRAY_SIZE(sent); i++) {
        kona_command(s, 0x14, sent[i], ARRAY_SIZE(sent[i]));
    }
    kona_command(s, 0x24, (uint32_t[]){ 0xc60 }, 1);
    assert_textport_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    activated = strstr(found, "sgi_kona_context_activate token 0x1fd");
    g_assert_nonnull(activated);
    g_assert_false(log_shows_misframing(activated));
    g_assert_cmpuint(count_occurrences(activated,
                                       "token 0x14 argc 9 consumed"),
                     ==, 0);
    g_assert_null(strstr(activated, "glTexGen of coordinate"));
    g_assert_nonnull(strstr(activated, "token 0x14 args 0x2001 0x2502 0x0"
                                       " argc 9\n"));
    g_assert_nonnull(strstr(activated, "token 0x24 args 0xc60 0x0 0x0"
                                       " argc 1\n"));
}

/*
 * A packed index whose bits 23:0 are zero fits both glIndexs and
 * glIndexub. The model reads it as the short and says so once, so a
 * glIndexub client's wrong colours stay diagnosable; a word only the
 * short sender makes, 0x01230000, is not reported.
 */
static void test_packed_index_short_reported_once(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    context_a_with_filled_page(s);
    kona_command(s, 0x75, (uint32_t[]){ 0x01230000 }, 1);
    kona_command(s, 0x75, (uint32_t[]){ 0x02000000 }, 1);
    kona_command(s, 0x75, (uint32_t[]){ 0x12000000 }, 1);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "read as the short"), ==, 1);
    g_assert_nonnull(strstr(found, "packed index 0x02000000 fits glIndexs"));
}

/*
 * A retrace request, 0x1e7 and its tag, completes at the next retrace:
 * the tag register reads the tag and the video cause bit 0x8 rises, with
 * the general vector sent, which is what runs kona_video_intr and its
 * kona_swap_complete. Before the retrace neither has happened, and a
 * retrace without a request rearms the periodic cause. Two requests complete
 * one per retrace, in order. The clock starts at 0, so the first 20 ms step
 * crosses the retrace at 16.7 ms and the next the one at 33.3 ms.
 */
static void test_retrace_request_completes(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    qtest_writel(s, REG_INT_VECTORS, KERNEL_INT_VECTORS);
    qtest_writel(s, REG_INT_DEST_UPPER, KERNEL_INT_DEST_UPPER);
    qtest_writel(s, REG_INT_DEST_LOWER, KERNEL_INT_DEST_LOWER);
    qtest_writel(s, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    context_a_with_filled_page(s);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 5 }, 1);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 0);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_VIDEO, ==, 0);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 5);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_VIDEO, ==,
                    INT_CAUSE_VIDEO);
    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_VIDEO);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_VIDEO, ==, 0);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_VIDEO, ==,
                    INT_CAUSE_VIDEO);

    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_VIDEO);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 6 }, 1);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 7 }, 1);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 6);
    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_VIDEO);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 7);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & ~INT_CAUSE_VIDEO, ==, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_false(log_shows_misframing(found));
    g_assert_nonnull(strstr(found, "sgi_kona_retrace_request tag 0x5"
                                   " context 0\n"));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_general_interrupt "
                                              "fabric 0x9800001800090"
                                              " vector 0x10\n"), ==, 4);
}

/*
 * A retrace request with no context current, as a retrace wait outside a
 * swap or a request before any activation sends it, still completes its
 * tag at the next retrace: the driver waits on the tag whatever it was
 * for. With no interrupt destination programmed the vector is logged,
 * not sent, and the cause bit still rises for a poll.
 */
static void test_retrace_request_without_context(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(&log);

    qtest_writel(s, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 9 }, 1);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 9);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, INT_CAUSE_VIDEO);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "sgi_kona_retrace_request tag 0x9"
                                   " context -1\n"));
    g_assert_nonnull(strstr(found, "general interrupt with no destination"
                                   " programmed"));
}

static void assert_video_vectors(const char *log, unsigned expected)
{
    g_autofree char *found = NULL;

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_general_interrupt "),
                     ==, expected);
}

static void test_periodic_video_mask(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    qtest_writel(s, REG_INT_VECTORS, KERNEL_INT_VECTORS);
    qtest_writel(s, REG_INT_DEST_UPPER, KERNEL_INT_DEST_UPPER);
    qtest_writel(s, REG_INT_DEST_LOWER, KERNEL_INT_DEST_LOWER);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, INT_CAUSE_VIDEO);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 0);
    assert_video_vectors(log, 0);
    qtest_writel(s, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    assert_video_vectors(log, 1);
    qtest_clock_step(s, RETRACE_STEP_NS);
    assert_video_vectors(log, 1);
    qtest_writel(s, KONA_REG(0x800230), 0);
    qtest_writel(s, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    assert_video_vectors(log, 1);
    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_VIDEO);
    qtest_clock_step(s, RETRACE_STEP_NS);
    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_VIDEO);
    assert_video_vectors(log, 2);
    qtest_writel(s, KONA_REG(0x800230), 0);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 0x8765 }, 1);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 0x8765);
    assert_video_vectors(log, 2);
    qtest_writel(s, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    qtest_clock_step(s, RETRACE_STEP_NS);
    assert_video_vectors(log, 3);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "sgi_kona_general_interrupt "),
                     ==, 3);
}

static void test_video_retry_destination(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(&log);

    qtest_writel(s, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    qtest_clock_step(s, RETRACE_STEP_NS);
    assert_video_vectors(log, 0);
    qtest_writel(s, REG_INT_VECTORS, KERNEL_INT_VECTORS);
    qtest_writel(s, REG_INT_DEST_UPPER, KERNEL_INT_DEST_UPPER);
    qtest_writel(s, REG_INT_DEST_LOWER, KERNEL_INT_DEST_LOWER);
    qtest_writel(s, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    assert_video_vectors(log, 1);
    qtest_clock_step(s, RETRACE_STEP_NS);
    assert_video_vectors(log, 1);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    qtest_clock_step(s, RETRACE_STEP_NS);
    assert_video_vectors(log, 1);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "general interrupt with no destination"));
}

static void test_video_delivery_migrates(void)
{
    g_autofree char *dir = NULL, *uri = NULL, *dest = NULL;
    g_autofree char *log_from = NULL, *log_to = NULL;
    g_autofree char *found_from = NULL, *found_to = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-kona-video-irq-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    from = kona_init_logged_args(MIGRATION_ARGS, &log_from);
    dest = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = kona_init_logged_args(dest, &log_to);
    qtest_writel(from, REG_INT_VECTORS, KERNEL_INT_VECTORS);
    qtest_writel(from, REG_INT_DEST_UPPER, KERNEL_INT_DEST_UPPER);
    qtest_writel(from, REG_INT_DEST_LOWER, KERNEL_INT_DEST_LOWER);
    qtest_writel(from, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    kona_command_kicked(from, 0x1e7, (uint32_t[]){ 0x8765 }, 1);
    qtest_clock_step(from, RETRACE_STEP_NS);
    assert_video_vectors(log_from, 1);
    sgi_migrate_stopped(from, uri);
    g_assert_cmphex(qtest_readl(to, REG_RETRACE_TAG), ==, 0x8765);
    qtest_writel(to, KONA_REG(0x800230), 0);
    qtest_writel(to, KONA_REG(0x800230), INT_CAUSE_VIDEO);
    qtest_clock_step(to, RETRACE_STEP_NS);
    assert_video_vectors(log_to, 0);
    qtest_writel(to, REG_INT_CAUSE, INT_CAUSE_VIDEO);
    qtest_clock_step(to, RETRACE_STEP_NS);
    assert_video_vectors(log_to, 1);
    qtest_quit(to);
    qtest_quit(from);
    found_from = kona_log_read(log_from);
    found_to = kona_log_read(log_to);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/origin2000/kona-context/video-retry-destination",
                   test_video_retry_destination);
    qtest_add_func("sn0/origin2000/kona-context/video-delivery-migrates",
                   test_video_delivery_migrates);
    qtest_add_func("sn0/origin2000/kona-context/periodic-video-mask",
                   test_periodic_video_mask);

    qtest_add_func("sn0/origin2000/kona-context/swap-packets-framed",
                   test_swap_packets_framed);
    qtest_add_func("sn0/origin2000/kona-context/make-current-stream-framed",
                   test_make_current_stream_framed);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "scissor-beside-graphics-info",
                   test_scissor_beside_graphics_info);
    qtest_add_func("sn0/origin2000/kona-context/tex-table-cut-by-slot-store",
                   test_tex_table_cut_by_slot_store);
    qtest_add_func("sn0/origin2000/kona-context/migration-mid-tex-table",
                   test_migration_mid_tex_table);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "deactivate-writes-save-area",
                   test_deactivate_writes_save_area);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "deactivate-writes-mapped-save-area",
                   test_deactivate_writes_mapped_save_area);
    qtest_add_func("sn0/origin2000/kona-context/return-page-per-context",
                   test_return_page_per_context);
    qtest_add_func("sn0/origin2000/kona-context/restore-unknown-context",
                   test_restore_unknown_context);
    qtest_add_func("sn0/origin2000/kona-context/context-table-retains-ten",
                   test_context_table_retains_ten);
    qtest_add_func("sn0/origin2000/kona-context/replaced-context-retrace",
                   test_replaced_context_retrace);
    qtest_add_func("sn0/origin2000/kona-context/unknown-preserves-allowance",
                   test_unknown_restore_preserves_allowance);
    qtest_add_func("sn0/origin2000/kona-context/renderer-reconstruction",
                   test_context_renderer_reconstruction);
    qtest_add_func("sn0/origin2000/kona-context/context-limit",
                   test_context_limit);
    qtest_add_func("sn0/origin2000/kona-context/migration-of-contexts",
                   test_migration_of_contexts);
    qtest_add_func("sn0/origin2000/kona-context/migration-unknown-selection",
                   test_migration_unknown_selection);
    qtest_add_func("sn0/origin2000/kona-context/migration-pending-page",
                   test_migration_pending_page);
    qtest_add_func("sn0/origin2000/kona-context/migration-replaced-retrace",
                   test_migration_replaced_retrace);
    qtest_add_func("sn0/origin2000/kona-context/context-allocation-failure",
                   test_context_allocation_failure);
    qtest_add_func("sn0/origin2000/kona-context/context-reinitialization",
                   test_context_reinitialization);
    qtest_add_func("sn0/origin2000/kona-context/reset-clears-pending-context",
                   test_reset_clears_pending_context);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "migration-rejects-context-stream",
                   test_migration_rejects_context_stream);
    qtest_add_func("sn0/origin2000/kona-context/retirement-retains-records",
                   test_context_retirement_retains_records);
    qtest_add_func("sn0/origin2000/kona-context/failed-context-registration",
                   test_failed_context_registration);
    qtest_add_func("sn0/origin2000/kona-context/line-distance-answer",
                   test_line_distance_answer);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "line-distance-on-current-context",
                   test_line_distance_on_current_context);
    qtest_add_func("sn0/origin2000/kona-context/get-error-answer",
                   test_get_error_answer);
    qtest_add_func("sn0/origin2000/kona-context/raster-query-answer",
                   test_raster_query_answer);
    qtest_add_func("sn0/origin2000/kona-context/current-query-answer",
                   test_current_query_answer);
    qtest_add_func("sn0/origin2000/kona-context/finish-sets-word-0",
                   test_finish_sets_word_0);
    qtest_add_func("sn0/origin2000/kona-context/swap-finish-posts-nothing",
                   test_swap_finish_posts_nothing);
    qtest_add_func("sn0/origin2000/kona-context/clear-stream-framed",
                   test_clear_stream_framed);
    qtest_add_func("sn0/origin2000/kona-context/triangle-stream-framed",
                   test_triangle_stream_framed);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "buffer-config-after-activation",
                   test_buffer_config_after_activation);
    qtest_add_func("sn0/origin2000/kona-context/tex-table-usage-framed",
                   test_tex_table_usage_framed);
    qtest_add_func("sn0/origin2000/kona-context/igloo-interface-framed",
                   test_igloo_interface_framed);
    qtest_add_func("sn0/origin2000/kona-context/tex-gen-framed",
                   test_tex_gen_framed);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "packed-index-short-reported-once",
                   test_packed_index_short_reported_once);
    qtest_add_func("sn0/origin2000/kona-context/retrace-request-completes",
                   test_retrace_request_completes);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "retrace-request-without-context",
                   test_retrace_request_without_context);

    qtest_add_func("sn0/origin2000/kona-context/deactivate-mid-user-command",
                   test_deactivate_mid_user_command);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "suspended-command-survives-other-context",
                   test_suspended_command_survives_other_context);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "suspended-x-write-survives-other-context",
                   test_suspended_x_write_survives_other_context);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "migration-of-suspended-x-write",
                   test_migration_of_suspended_x_write);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "migration-rejects-x-pixel-owner",
                   test_migration_rejects_x_pixel_owner);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "x-dma-for-other-context-logged",
                   test_x_dma_for_other_context_logged);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "suspended-bitmap-survives-other-context",
                   test_suspended_bitmap_survives_other_context);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "suspended-gl-run-survives-other-context",
                   test_suspended_gl_run_survives_other_context);
    qtest_add_func("sn0/origin2000/kona-context/migration-of-saved-command",
                   test_migration_of_saved_command);
    qtest_add_func("sn0/origin2000/kona-context/migration-of-saved-flash-op",
                   test_migration_of_saved_flash_op);
    qtest_add_func("sn0/origin2000/kona-context/list-call-split-by-switch",
                   test_list_call_split_by_switch);
    qtest_add_func("sn0/origin2000/kona-context/x-command-split-by-switch",
                   test_x_command_split_by_switch);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "x-data-run-survives-other-context",
                   test_x_data_run_survives_other_context);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "x-clip-run-survives-other-context",
                   test_x_clip_run_survives_other_context);
    qtest_add_func("sn0/origin2000/kona-context/x-data-run-owner-rules",
                   test_x_data_run_owner_rules);
    qtest_add_func("sn0/origin2000/kona-context/migration-of-x-data-run",
                   test_migration_of_x_data_run);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "x-run-taken-by-other-context-logged",
                   test_x_run_taken_by_other_context_logged);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "privileged-command-cut-by-force",
                   test_privileged_command_cut_by_force);
    qtest_add_data_func("sn0/origin2000/kona-context/"
                        "migration-of-suspended-list-call",
                        GINT_TO_POINTER(false),
                        test_migration_of_kernel_port_command);
    qtest_add_data_func("sn0/origin2000/kona-context/"
                        "migration-of-saved-list-call",
                        GINT_TO_POINTER(true),
                        test_migration_of_kernel_port_command);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "migration-rejects-bitmap-stream",
                   test_migration_rejects_bitmap_stream);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "suspended-texture-load-survives",
                   test_suspended_texture_load_survives);
    qtest_add_func("sn0/origin2000/kona-context/migration-of-saved-gl-run",
                   test_migration_of_saved_gl_run);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "migration-rejects-saved-gl-run",
                   test_migration_rejects_saved_gl_run);
    qtest_add_func("sn0/origin2000/kona-context/migration-of-saved-bitmap",
                   test_migration_of_saved_bitmap);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "fresh-context-discards-saved-command",
                   test_fresh_context_discards_saved_command);
    qtest_add_func("sn0/origin2000/kona-context/migration-rejects-saved-input",
                   test_migration_rejects_saved_input);
    qtest_add_func("sn0/origin2000/kona-context/reply-after-deactivation",
                   test_reply_after_deactivation);
    return g_test_run();
}
