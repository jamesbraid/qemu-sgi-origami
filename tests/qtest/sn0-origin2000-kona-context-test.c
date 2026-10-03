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
#define SAVE_BASE           UINT64_C(0x400000)
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
#define CONTEXTS            8

/* The texture table's data words after its slot word. */
#define TEX_TABLE_WORDS     33

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

/* The one-node machine is small enough to migrate quickly as it is. */
#define MIGRATION_ARGS MACHINE_ARGS

/*
 * A fresh instance tracing every sgi_kona event and logging guest errors
 * and unimplemented accesses to its own file. The caller quits it and
 * reads the log with kona_log_read(), which also removes the file.
 */
static QTestState *kona_init_logged(char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-context-XXXXXX.log",
                           g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    return qtest_initf("%s -trace enable=sgi_kona_* -d guest_errors,unimp"
                       " -D %s", MACHINE_ARGS, *log);
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
 * A texture table as bindTexTable and __glkon_datavn send it: 0x17e and
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
    g_assert_nonnull(strstr(found, "sgi_kona_gl_window token 0x1f3 slot 0"
                                   " (1, 9) "));
    g_assert_nonnull(strstr(found, "sgi_kona_gl_window token 0x1ee slot 0"
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
    g_assert_cmpuint(count_occurrences(found, "token 0x17e argc 34 consumed"),
                     ==, 2);
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

/*
 * The model keeps eight contexts. A ninth takes the entry of the one
 * activated longest ago, and says so; the second context, still held,
 * answers on its own page when restored, and the forgotten first one
 * answers nowhere, its restore taking another entry in turn.
 */
static void test_context_table_evicts_oldest(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *evicted = NULL;
    QTestState *s = kona_init_logged(&log);

    kona_register_reason_page(s, REASON_PAGE);
    for (unsigned i = 0; i <= CONTEXTS; i++) {
        guest_fill_word(s, RETURN_BASE + 0x4000 * i, 0);
        context_first_physical(s, SAVE_BASE + 0x10000 * i,
                               RETURN_BASE + 0x4000 * i);
        context_deactivate(s, i);
    }
    context_restore_physical(s, SAVE_BASE + 0x10000);
    x_initialise(s, 1280);
    g_assert_cmphex(guest_word(s, RETURN_BASE + 0x4000), ==, 1);
    context_deactivate(s, 1);
    context_restore_physical(s, SAVE_BASE);
    x_initialise(s, 640);
    g_assert_cmphex(guest_word(s, RETURN_BASE), ==, 0);
    qtest_quit(s);

    found = kona_log_read(log);
    evicted = g_strdup_printf("context table full (8 entries); forgetting"
                              " the context with save area 0x%" PRIx64,
                              (uint64_t)physical_page_word(SAVE_BASE) << 12);
    g_assert_nonnull(strstr(found, evicted));
    g_assert_cmpuint(count_occurrences(found, "context table full"), ==, 2);
    g_assert_nonnull(strstr(found, "which no 0x1fd set up"));
}

/*
 * The context table migrates: two contexts set up and switched out on the
 * source, then on the destination a restore of A answers on A's page, and
 * a deactivate writes A's save area.
 */
static void test_migration_of_contexts(void)
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

    guest_fill_word(from, RETURN_A, 0);
    guest_fill_word(from, RETURN_B, 0);
    kona_register_reason_page(from, REASON_PAGE);
    context_first_physical(from, SAVE_A, RETURN_A);
    context_deactivate(from, 1);
    context_first_physical(from, SAVE_B, RETURN_B);
    context_deactivate(from, 2);

    sgi_migrate_stopped(from, uri);

    context_restore_physical(to, SAVE_A);
    x_initialise(to, 1280);
    g_assert_cmphex(guest_word(to, RETURN_A), ==, 1);
    g_assert_cmphex(guest_word(to, RETURN_B), ==, 0);
    guest_fill_word(to, SAVE_A, 0);
    context_deactivate(to, 1);
    g_assert_cmphex(guest_word(to, SAVE_A), ==, SAVE_MARKER);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
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
    g_assert_nonnull(strstr(activated, "token 0x114 argc 1 consumed"));
    g_assert_nonnull(strstr(activated, "sgi_kona_gl_buffer_config token 0x4"
                                       " level 0 rgb 12 index 0 double 0\n"));
    g_assert_nonnull(strstr(activated, "token 0x4 argc 10 consumed"));
    g_assert_null(strstr(activated, "token 0xc args"));
    g_assert_nonnull(strstr(activated, "token 0x177 args 0x77000000 0x0 0x0"
                                       " argc 1\n"));
}

/*
 * A retrace request, 0x1e7 and its tag, completes at the next retrace:
 * the tag register reads the tag and the video cause bit 0x8 rises, with
 * the general vector sent, which is what runs kona_video_intr and its
 * kona_swap_complete. Before the retrace neither has happened, and a
 * retrace with nothing to honour raises nothing. Two requests complete one
 * per retrace, in order. The clock starts at 0, so the first 20 ms step
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
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_VIDEO, ==, 0);

    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 6 }, 1);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ 7 }, 1);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(s, REG_RETRACE_TAG), ==, 6);
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
                                              " vector 0x10\n"), ==, 3);
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

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
    qtest_add_func("sn0/origin2000/kona-context/context-table-evicts-oldest",
                   test_context_table_evicts_oldest);
    qtest_add_func("sn0/origin2000/kona-context/migration-of-contexts",
                   test_migration_of_contexts);
    qtest_add_func("sn0/origin2000/kona-context/line-distance-answer",
                   test_line_distance_answer);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "line-distance-on-current-context",
                   test_line_distance_on_current_context);
    qtest_add_func("sn0/origin2000/kona-context/get-error-answer",
                   test_get_error_answer);
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
    qtest_add_func("sn0/origin2000/kona-context/retrace-request-completes",
                   test_retrace_request_completes);
    qtest_add_func("sn0/origin2000/kona-context/"
                   "retrace-request-without-context",
                   test_retrace_request_without_context);

    return g_test_run();
}
