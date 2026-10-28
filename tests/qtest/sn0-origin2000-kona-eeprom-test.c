/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA flash EEPROM operation qtests.
 *
 * The pipe's flash holds its boot image, a video combination and a 48-byte
 * header. IRIX's EEPROM programming tool reads and programs it with command
 * 0x1ec, an operation number and that operation's words, and reads the pipe's
 * answer from a reply buffer: word 0 goes non-zero last, word 1 is a word count
 * and the data follows. The PROM and the kernel read the same header through
 * the indirect port at internal 0x87ff00. The contract is the command grammar
 * datasheet's "The flash EEPROM operation, 0x1ec, and the EEPROM programming
 * tool". Each case fails on a model without the behaviour for the reason its
 * comment gives.
 *
 * The fixture is sn0-origin2000-kona-bringup-test.c's: KONA as a -device
 * at the empty slot IO4, crossbow0's widget 11, with its fabric writes
 * reaching node 0's memory.
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

#define REG_PORT_ADDR       KONA_REG(0x8003e8)
#define REG_PORT_DATA       KONA_REG(0x8003f0)
#define REG_INT_CAUSE       KONA_REG(0x800238)
#define REG_MAILBOX         KONA_REG(0x800240)
#define REG_COMMAND         KONA_REG(0xc10000)

#define INT_CAUSE_MAILBOX   0x10

#define CMD_EEPROM          0x1ec
#define OP_WRITE            1
#define OP_READ             2
#define OP_REPLY_BUFFER     3
#define OP_CHECK            4
#define OP_IDENTIFY         5
#define OP_ERASE_COMBO      6
#define TYPE_IMAGE          0
#define TYPE_COMBO          1
#define TYPE_HEADER         2

#define FLASH_COMBO         0x20000
#define FLASH_HEADER        0x7ff00
#define HEADER_WORDS        12
#define HEADER_STATE        7
/* The indirect port reaches the flash's read side here. */
#define INTERNAL_FLASH      0x800000

/* The manual's example identity: manufacturer 0x1f, device 0xc4. */
#define FLASH_IDENTITY      0x1fc40000

/* A node 0 guest RAM page for replies, and a second one. */
#define REPLY_PAGE          UINT64_C(0x320000)
#define OTHER_PAGE          UINT64_C(0x330000)

#define MACHINE_ARGS \
    "-M origin2000,nodes=1,x-uncatalogued-graphics=on -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

static QTestState *qts;

static uint32_t port_peek(QTestState *s, uint32_t addr)
{
    qtest_writel(s, REG_PORT_ADDR, addr);
    return qtest_readl(s, REG_PORT_DATA);
}

static void words_out(QTestState *s, const uint32_t *words, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        qtest_writel(s, REG_COMMAND, words[i]);
    }
}

static uint32_t guest_word(QTestState *s, uint64_t addr)
{
    uint8_t bytes[4];

    qtest_memread(s, addr, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

static void guest_clear(QTestState *s, uint64_t addr, size_t len)
{
    g_autofree uint8_t *zero = g_malloc0(len);

    qtest_memwrite(s, addr, zero, len);
}

/*
 * the EEPROM programming tool's unmanaged-path reply buffer: 0x1ec 3, the page
 * word with the host-physical bit and the offset word as ioctl 0x2774 op 9
 * returns them, and the buffer length.
 */
static void reply_buffer(QTestState *s, uint64_t addr)
{
    words_out(s, (uint32_t[]){ CMD_EEPROM, OP_REPLY_BUFFER,
                               0x10000000 | (uint32_t)(addr >> 12),
                               (uint32_t)(addr & 0xfff), 0x4000 }, 5);
}

/* The reply at `addr`: word 0 must be set; returns the count, word 1. */
static uint32_t reply_count(QTestState *s, uint64_t addr)
{
    g_assert_cmphex(guest_word(s, addr), ==, 1);
    return guest_word(s, addr + 4);
}

static uint32_t reply_data(QTestState *s, uint64_t addr, unsigned i)
{
    return guest_word(s, addr + 8 + 4 * i);
}

/* Op 1: a type, the offset for type 0 only, a count, then the words. */
static void flash_write(QTestState *s, uint32_t type, uint32_t offset,
                        const uint32_t *words, uint32_t count)
{
    qtest_writel(s, REG_COMMAND, CMD_EEPROM);
    qtest_writel(s, REG_COMMAND, OP_WRITE);
    qtest_writel(s, REG_COMMAND, type);
    if (type == TYPE_IMAGE) {
        qtest_writel(s, REG_COMMAND, offset);
    }
    qtest_writel(s, REG_COMMAND, count);
    words_out(s, words, count);
}

static void flash_read(QTestState *s, uint32_t offset, uint32_t bytes)
{
    words_out(s, (uint32_t[]){ CMD_EEPROM, OP_READ, offset, bytes }, 4);
}

/*
 * The PROM's sync probe, 1 9 0: its mailbox reply shows the command page
 * is between commands, so no word of an earlier operation is still owed.
 */
static void assert_command_page_in_step(QTestState *s)
{
    words_out(s, (uint32_t[]){ 1, 9, 0 }, 3);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==,
                    INT_CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x1234abcd);
    qtest_writel(s, REG_MAILBOX, 0);
    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_MAILBOX);
}

/*
 * the EEPROM programming tool's check, 0x1ec 4 0, answers count 1 and a
 * non-zero status in the buffer 0x1ec 3 named, at its byte offset: the EEPROM
 * programming tool polls word 0 and then needs word 2 non-zero, and without it
 * prints "graphics not responding". Fails on a model that does not frame 0x1ec,
 * does not post, or drops the offset's low 12 bits as 0x1f2 does on the pipe.
 */
static void test_check_answers_at_offset(void)
{
    uint64_t at = REPLY_PAGE + 0x100;

    qtest_system_reset(qts);
    guest_clear(qts, REPLY_PAGE, 0x200);
    reply_buffer(qts, at);
    words_out(qts, (uint32_t[]){ CMD_EEPROM, OP_CHECK, 0 }, 3);
    g_assert_cmpuint(reply_count(qts, at), ==, 1);
    g_assert_cmphex(reply_data(qts, at, 0), ==, 1);
    g_assert_cmphex(guest_word(qts, REPLY_PAGE), ==, 0);
    assert_command_page_in_step(qts);
}

/* 0x1ec 5 0: count 1, the identity word. */
static void test_identify(void)
{
    qtest_system_reset(qts);
    guest_clear(qts, REPLY_PAGE, 0x100);
    reply_buffer(qts, REPLY_PAGE);
    words_out(qts, (uint32_t[]){ CMD_EEPROM, OP_IDENTIFY, 0 }, 3);
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, 1);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, FLASH_IDENTITY);
    assert_command_page_in_step(qts);
}

/*
 * The default flash: zero, but the header's state reads 2, through op 2
 * and through the indirect port the PROM and kernel use. A model that
 * kept a fixed answer at one port address only would fail the op 2 half.
 */
static void test_default_header(void)
{
    /* Its own machine: the shared one's flash keeps other cases' writes. */
    QTestState *s = qtest_init(MACHINE_ARGS);

    guest_clear(s, REPLY_PAGE, 0x100);
    reply_buffer(s, REPLY_PAGE);
    flash_read(s, FLASH_HEADER, HEADER_WORDS * 4);
    g_assert_cmpuint(reply_count(s, REPLY_PAGE), ==, HEADER_WORDS);
    for (unsigned i = 0; i < HEADER_WORDS; i++) {
        uint32_t want = i == HEADER_STATE ? 2 : 0;

        g_assert_cmphex(reply_data(s, REPLY_PAGE, i), ==, want);
        g_assert_cmphex(port_peek(s, INTERNAL_FLASH + FLASH_HEADER + 4 * i),
                        ==, want);
    }
    qtest_quit(s);
}

/*
 * the EEPROM programming tool's header write, 0x1ec 1 2 12 and twelve words:
 * status 1, and the words read back through op 2 and through the indirect port
 * at 0x87ff00, so a header written with state 3 is what the PROM's check sees
 * on the next boot.
 */
static void test_header_write_reaches_port(void)
{
    uint32_t header[HEADER_WORDS];

    for (unsigned i = 0; i < HEADER_WORDS; i++) {
        header[i] = 0x11111111 * (i + 1);
    }
    header[HEADER_STATE] = 3;

    qtest_system_reset(qts);
    guest_clear(qts, REPLY_PAGE, 0x100);
    reply_buffer(qts, REPLY_PAGE);
    flash_write(qts, TYPE_HEADER, 0, header, HEADER_WORDS);
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, 1);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, 1);

    guest_clear(qts, REPLY_PAGE, 0x100);
    flash_read(qts, FLASH_HEADER, sizeof(header));
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, HEADER_WORDS);
    for (unsigned i = 0; i < HEADER_WORDS; i++) {
        g_assert_cmphex(reply_data(qts, REPLY_PAGE, i), ==, header[i]);
        g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + FLASH_HEADER + 4 * i),
                        ==, header[i]);
    }
    assert_command_page_in_step(qts);
}

/*
 * An image group, 0x1ec 1 0 a n and n words, reads back from a with op 2,
 * including at an offset op 2 reads unaligned to the group.
 */
static void test_image_write_reads_back(void)
{
    enum { N = 64 };
    uint32_t words[N];

    for (unsigned i = 0; i < N; i++) {
        words[i] = 0xa5000000 | (i * 0x10203);
    }

    qtest_system_reset(qts);
    guest_clear(qts, REPLY_PAGE, 0x400);
    reply_buffer(qts, REPLY_PAGE);
    flash_write(qts, TYPE_IMAGE, 0x4000, words, N);
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, 1);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, 1);

    guest_clear(qts, REPLY_PAGE, 0x400);
    flash_read(qts, 0x4000, N * 4);
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, N);
    for (unsigned i = 0; i < N; i++) {
        g_assert_cmphex(reply_data(qts, REPLY_PAGE, i), ==, words[i]);
    }

    guest_clear(qts, REPLY_PAGE, 0x400);
    flash_read(qts, 0x4000 + 8, 8);
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, 2);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, words[2]);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 1), ==, words[3]);
    assert_command_page_in_step(qts);
}

/*
 * A count above 0x7ffb fails before any data word is read: status 0, and
 * the next word starts a new command. A model that took the count's words
 * would swallow the sync probe that follows.
 */
static void test_oversized_count_refused(void)
{
    qtest_system_reset(qts);
    guest_clear(qts, REPLY_PAGE, 0x100);
    reply_buffer(qts, REPLY_PAGE);
    words_out(qts, (uint32_t[]){ CMD_EEPROM, OP_WRITE, TYPE_IMAGE, 0,
                                 0x7ffc }, 5);
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, 1);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, 0);
    assert_command_page_in_step(qts);
}

/*
 * A combination, 0x1ec 1 1 n and n words, lands at 0x20000 after the
 * marker 0xfacefeed and n; 0x1ec 6, which takes no word and posts nothing,
 * zeroes the marker.
 */
static void test_combination_and_its_erase(void)
{
    static const uint32_t words[] = { 0xc0ffee01, 0xc0ffee02, 0xc0ffee03 };

    qtest_system_reset(qts);
    guest_clear(qts, REPLY_PAGE, 0x100);
    reply_buffer(qts, REPLY_PAGE);
    flash_write(qts, TYPE_COMBO, 0, words, ARRAY_SIZE(words));
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, 1);

    g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + FLASH_COMBO), ==,
                    0xfacefeed);
    g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + FLASH_COMBO + 4), ==,
                    ARRAY_SIZE(words));
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + FLASH_COMBO + 8 +
                                  4 * i), ==, words[i]);
    }

    guest_clear(qts, REPLY_PAGE, 0x100);
    words_out(qts, (uint32_t[]){ CMD_EEPROM, OP_ERASE_COMBO }, 2);
    g_assert_cmphex(guest_word(qts, REPLY_PAGE), ==, 0);
    g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + FLASH_COMBO), ==, 0);
    g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + FLASH_COMBO + 4), ==,
                    ARRAY_SIZE(words));
    assert_command_page_in_step(qts);
}

/*
 * The check after a refused or ignored request: it answers 1 at once, so
 * the command page took no stray word of the request as its own.
 */
static void assert_check_answers(QTestState *s, uint64_t at)
{
    guest_clear(s, at, 0x10);
    words_out(s, (uint32_t[]){ CMD_EEPROM, OP_CHECK, 0 }, 3);
    g_assert_cmpuint(reply_count(s, at), ==, 1);
    g_assert_cmphex(reply_data(s, at, 0), ==, 1);
}

/*
 * An operation number outside 1 to 6 takes no more words: the ARM returns
 * having read nothing more. The check that follows answers, which fails
 * on a model that swallowed its 0x1ec as the unknown operation's word.
 */
static void test_unknown_op_takes_nothing(void)
{
    qtest_system_reset(qts);
    reply_buffer(qts, REPLY_PAGE);
    words_out(qts, (uint32_t[]){ CMD_EEPROM, 7 }, 2);
    assert_check_answers(qts, REPLY_PAGE);
    words_out(qts, (uint32_t[]){ CMD_EEPROM, 0 }, 2);
    assert_check_answers(qts, REPLY_PAGE);
    assert_command_page_in_step(qts);
}

/*
 * A write of an unknown type takes its count's data words, stores none of
 * them and answers 0.
 */
static void test_unknown_type_consumes_data(void)
{
    static const uint32_t words[] = { 0x0badda7a, 0x0badda7b };

    qtest_system_reset(qts);
    guest_clear(qts, REPLY_PAGE, 0x100);
    reply_buffer(qts, REPLY_PAGE);
    words_out(qts, (uint32_t[]){ CMD_EEPROM, OP_WRITE, 3,
                                 ARRAY_SIZE(words) }, 4);
    words_out(qts, words, ARRAY_SIZE(words));
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, 1);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, 0);
    for (uint32_t off = 0; off < 0x10; off += 4) {
        g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + off), !=, words[0]);
        g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + off), !=, words[1]);
    }
    assert_check_answers(qts, REPLY_PAGE);
    assert_command_page_in_step(qts);
}

/*
 * Reads the model refuses: a length that is not whole words, which would
 * never end the ARM's copy loop, and a reply longer than the 16 KiB
 * buffer. Each takes its two words, posts nothing, and leaves the page in
 * step.
 */
static void test_refused_reads_post_nothing(void)
{
    static const uint32_t lengths[] = { 6, 0x4000 - 4, 0x4000 };

    qtest_system_reset(qts);
    reply_buffer(qts, REPLY_PAGE);
    for (unsigned i = 0; i < ARRAY_SIZE(lengths); i++) {
        guest_clear(qts, REPLY_PAGE, 0x10);
        flash_read(qts, 0, lengths[i]);
        g_assert_cmphex(guest_word(qts, REPLY_PAGE), ==, 0);
        assert_check_answers(qts, REPLY_PAGE);
    }
    /* The longest read that fits: count plus 0xff8 data words. */
    guest_clear(qts, REPLY_PAGE, 0x10);
    flash_read(qts, 0, 0x4000 - 8);
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, (0x4000 - 8) / 4);
    assert_command_page_in_step(qts);
}

/*
 * A read running past the 512 KiB answers the words inside it and, as the
 * model's choice, zero past it; the count is the whole request.
 */
static void test_read_past_end_answers_zero(void)
{
    static const uint32_t words[] = { 0xe0d0f000, 0xe0d0f004 };

    qtest_system_reset(qts);
    reply_buffer(qts, OTHER_PAGE);
    flash_write(qts, TYPE_IMAGE, 0x7fff8, words, ARRAY_SIZE(words));
    guest_clear(qts, REPLY_PAGE, 0x100);
    reply_buffer(qts, REPLY_PAGE);
    flash_read(qts, 0x7fff8, 16);
    g_assert_cmpuint(reply_count(qts, REPLY_PAGE), ==, 4);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, words[0]);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 1), ==, words[1]);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 2), ==, 0);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 3), ==, 0);
    /* An offset op 2 rounds down to a word. */
    guest_clear(qts, REPLY_PAGE, 0x100);
    flash_read(qts, 0x7fff8 + 3, 4);
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, words[0]);
    assert_command_page_in_step(qts);
}

/*
 * A write running past the 512 KiB stores the bytes inside it, drops the
 * rest, answers 1, and does not wrap round to offset 0 even from an offset
 * near 2^32.
 */
static void test_write_past_end_dropped(void)
{
    static const uint32_t words[] = { 0x5a5a0001, 0x5a5a0002 };

    qtest_system_reset(qts);
    guest_clear(qts, REPLY_PAGE, 0x100);
    reply_buffer(qts, REPLY_PAGE);
    flash_write(qts, TYPE_IMAGE, 0x7fffc, words, ARRAY_SIZE(words));
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, 1);
    g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + 0x7fffc), ==, words[0]);

    guest_clear(qts, REPLY_PAGE, 0x100);
    flash_write(qts, TYPE_IMAGE, 0xfffffffc, words, ARRAY_SIZE(words));
    g_assert_cmphex(reply_data(qts, REPLY_PAGE, 0), ==, 1);
    g_assert_cmphex(port_peek(qts, INTERNAL_FLASH), !=, words[1]);
    assert_command_page_in_step(qts);
}

/*
 * The flash is non-volatile: a written word survives a system reset,
 * where the reply buffer does not.
 */
static void test_flash_survives_reset(void)
{
    static const uint32_t word = 0x5eed1e55;

    qtest_system_reset(qts);
    reply_buffer(qts, REPLY_PAGE);
    flash_write(qts, TYPE_IMAGE, 0x100, &word, 1);
    qtest_system_reset(qts);
    g_assert_cmphex(port_peek(qts, INTERNAL_FLASH + 0x100), ==, word);
}

/*
 * The flash crosses migration, and so does a write the source left part
 * sent: the destination takes its last words and answers it.
 */
static void test_migration(void)
{
    static const uint32_t words[] = { 0x01020304, 0x05060708, 0x090a0b0c };
    static const uint32_t written = 0x7e57ab1e;
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-eeprom-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    reply_buffer(from, OTHER_PAGE);
    flash_write(from, TYPE_IMAGE, 0x40, &written, 1);
    guest_clear(from, REPLY_PAGE, 0x100);
    reply_buffer(from, REPLY_PAGE);
    /* The group's command and its first word only. */
    words_out(from, (uint32_t[]){ CMD_EEPROM, OP_WRITE, TYPE_IMAGE, 0x80,
                                  ARRAY_SIZE(words), words[0] }, 6);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(port_peek(to, INTERNAL_FLASH + 0x40), ==, written);
    g_assert_cmphex(guest_word(to, REPLY_PAGE), ==, 0);
    words_out(to, words + 1, ARRAY_SIZE(words) - 1);
    g_assert_cmpuint(reply_count(to, REPLY_PAGE), ==, 1);
    g_assert_cmphex(reply_data(to, REPLY_PAGE, 0), ==, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        g_assert_cmphex(port_peek(to, INTERNAL_FLASH + 0x80 + 4 * i), ==,
                        words[i]);
    }
    assert_command_page_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona-eeprom/check-answers-at-offset",
                   test_check_answers_at_offset);
    qtest_add_func("sn0/origin2000/kona-eeprom/identify", test_identify);
    qtest_add_func("sn0/origin2000/kona-eeprom/default-header",
                   test_default_header);
    qtest_add_func("sn0/origin2000/kona-eeprom/header-write-reaches-port",
                   test_header_write_reaches_port);
    qtest_add_func("sn0/origin2000/kona-eeprom/image-write-reads-back",
                   test_image_write_reads_back);
    qtest_add_func("sn0/origin2000/kona-eeprom/oversized-count-refused",
                   test_oversized_count_refused);
    qtest_add_func("sn0/origin2000/kona-eeprom/combination-and-its-erase",
                   test_combination_and_its_erase);
    qtest_add_func("sn0/origin2000/kona-eeprom/unknown-op-takes-nothing",
                   test_unknown_op_takes_nothing);
    qtest_add_func("sn0/origin2000/kona-eeprom/unknown-type-consumes-data",
                   test_unknown_type_consumes_data);
    qtest_add_func("sn0/origin2000/kona-eeprom/refused-reads-post-nothing",
                   test_refused_reads_post_nothing);
    qtest_add_func("sn0/origin2000/kona-eeprom/read-past-end-answers-zero",
                   test_read_past_end_answers_zero);
    qtest_add_func("sn0/origin2000/kona-eeprom/write-past-end-dropped",
                   test_write_past_end_dropped);
    qtest_add_func("sn0/origin2000/kona-eeprom/flash-survives-reset",
                   test_flash_survives_reset);
    qtest_add_func("sn0/origin2000/kona-eeprom/migration", test_migration);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
