/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Check sgi-kona-ports.h for duplicate selectors and argument counts beyond
 * the framer limit. Drive every declared selector through SGICommandPort and
 * check its packet; sample deliberately excluded selectors on plausible
 * ports to catch accidental additions.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-command-port.h"
#include "hw/misc/sgi-kona-ports.h"

typedef struct RecordedPacket {
    bool got_packet;
    uint32_t selector;
    uint32_t argc;
    uint32_t args[SGI_COMMAND_PORT_MAX_ARGS];
    bool got_unsupported;
    uint32_t unsupported_word;
} RecordedPacket;

static void record_packet(void *opaque, unsigned port_id, uint32_t selector,
                           const uint32_t *args, unsigned argc)
{
    RecordedPacket *r = opaque;

    g_assert_false(r->got_packet);
    r->got_packet = true;
    r->selector = selector;
    r->argc = argc;
    memcpy(r->args, args, argc * sizeof(*args));
}

static void record_unsupported(void *opaque, unsigned port_id, uint32_t word)
{
    RecordedPacket *r = opaque;

    r->got_unsupported = true;
    r->unsupported_word = word;
}

static const SGICommandPortOps test_ops = {
    .packet = record_packet,
    .unsupported = record_unsupported,
};

/* No duplicate selector value and no argc past the framer's own bound. */
static void check_table_shape(const SGICommandPortSelector *selectors,
                               unsigned num_selectors)
{
    for (unsigned i = 0; i < num_selectors; i++) {
        g_assert_cmpuint(selectors[i].argc, <=, SGI_COMMAND_PORT_MAX_ARGS);
        for (unsigned j = i + 1; j < num_selectors; j++) {
            g_assert_cmphex(selectors[i].selector, !=, selectors[j].selector);
        }
    }
}

/*
 * Pushes selector then argc filler words and confirms the packet
 * callback fires with exactly that selector and argc.
 */
static void check_selector_frames(const SGICommandPortSelector *selectors,
                                   unsigned num_selectors, unsigned port_id)
{
    for (unsigned i = 0; i < num_selectors; i++) {
        RecordedPacket r = { 0 };
        SGICommandPort p;

        sgi_command_port_init(&p, &test_ops, &r, port_id,
                               selectors, num_selectors);
        sgi_command_port_write(&p, selectors[i].selector, 4);
        /*
         * A bare (argc == 0) selector frames its packet on this same
         * write; only a selector with arguments still pending should
         * leave the packet callback unfired at this point.
         */
        g_assert_true(r.got_packet == (selectors[i].argc == 0));
        for (unsigned arg = 0; arg < selectors[i].argc; arg++) {
            sgi_command_port_write(&p, 0x10000000u + arg, 4);
        }
        g_assert_true(r.got_packet);
        g_assert_false(r.got_unsupported);
        g_assert_cmphex(r.selector, ==, selectors[i].selector);
        g_assert_cmpuint(r.argc, ==, selectors[i].argc);
        for (unsigned arg = 0; arg < selectors[i].argc; arg++) {
            g_assert_cmphex(r.args[arg], ==, 0x10000000u + arg);
        }
    }
}

static void check_word_unsupported(const SGICommandPortSelector *selectors,
                                    unsigned num_selectors, unsigned port_id,
                                    uint32_t word)
{
    RecordedPacket r = { 0 };
    SGICommandPort p;

    sgi_command_port_init(&p, &test_ops, &r, port_id, selectors,
                           num_selectors);
    sgi_command_port_write(&p, word, 4);
    g_assert_true(r.got_unsupported);
    g_assert_cmphex(r.unsupported_word, ==, word);
    g_assert_false(r.got_packet);
}

/*
 * The DMA FIFO port's selectors and counts: KONAsetdisplaymode's 0x1e6
 * and the bare select-feed pair. Every other kernel packet's sender writes
 * the command page instead.
 */
static const SGICommandPortSelector expected_fifo_selectors[] = {
    { 0x1e6, 3 }, { 0x1f5, 0 }, { 0x1f6, 0 },
};

static void check_table_matches(const SGICommandPortSelector *selectors,
                                unsigned num_selectors,
                                const SGICommandPortSelector *expected,
                                size_t num_expected)
{
    g_assert_cmpuint(num_selectors, ==, num_expected);
    for (size_t i = 0; i < num_expected; i++) {
        bool found = false;

        for (unsigned j = 0; j < num_selectors; j++) {
            if (selectors[j].selector == expected[i].selector) {
                g_assert_cmpuint(selectors[j].argc, ==, expected[i].argc);
                found = true;
            }
        }
        g_assert_true(found);
    }
}

static void test_fifo_table_shape(void)
{
    check_table_shape(sgi_kona_fifo_selectors, sgi_kona_fifo_num_selectors);
    check_table_matches(sgi_kona_fifo_selectors, sgi_kona_fifo_num_selectors,
                        expected_fifo_selectors,
                        ARRAY_SIZE(expected_fifo_selectors));
}

static void test_fifo_selectors_frame(void)
{
    check_selector_frames(sgi_kona_fifo_selectors,
                           sgi_kona_fifo_num_selectors, SGI_KONA_PORT_FIFO);
}

/*
 * Selectors the header's comment excludes for a stated reason:
 * dual-port ambiguity (0x100), and the kernel packets the model receives
 * on the command page rather than this FIFO port, framed there by
 * sgi_kona_command_tokens[]. The DMA engine frames 0x1eb and 0x1c8 on its
 * own port with its own table. None may appear in the
 * shipped table.
 */
static void test_fifo_excluded_selectors(void)
{
    static const uint32_t excluded[] = {
        0x1fd, 0x1f3, 0x172, 0x100, 0x1eb, 0x1ee, 0x1f9, 0x1f2, 0x1f4,
        0x1fc, 0x1ed, 0x1c8,
    };

    for (size_t i = 0; i < ARRAY_SIZE(excluded); i++) {
        check_word_unsupported(sgi_kona_fifo_selectors,
                                sgi_kona_fifo_num_selectors,
                                SGI_KONA_PORT_FIFO, excluded[i]);
    }
}

static void test_textport_table_shape(void)
{
    check_table_shape(sgi_kona_textport_selectors,
                       sgi_kona_textport_num_selectors);
}

static void test_textport_selectors_frame(void)
{
    check_selector_frames(sgi_kona_textport_selectors,
                           sgi_kona_textport_num_selectors,
                           SGI_KONA_PORT_TEXTPORT);
}

/*
 * Code 0xa is the variable-length bulk transfer the header excludes;
 * it must not appear as a fixed-argc entry in this table.
 */
static void test_textport_excludes_bulk_transfer(void)
{
    check_word_unsupported(sgi_kona_textport_selectors,
                            sgi_kona_textport_num_selectors,
                            SGI_KONA_PORT_TEXTPORT, 0xa);
}

static void test_doorbell_table_shape(void)
{
    check_table_shape(sgi_kona_doorbell_selectors,
                       sgi_kona_doorbell_num_selectors);
}

static void test_doorbell_selectors_frame(void)
{
    check_selector_frames(sgi_kona_doorbell_selectors,
                           sgi_kona_doorbell_num_selectors,
                           SGI_KONA_PORT_DOORBELL);
}

/*
 * These selectors have unresolved argument counts and remain excluded from
 * the fixed-count doorbell table.
 */
static void test_doorbell_excluded_selectors(void)
{
    static const uint32_t excluded[] = {
        0x0, 0x500, 0x1a, 0x1f, 0x8c, 0x90, 0x39, 0x119,
    };

    for (size_t i = 0; i < ARRAY_SIZE(excluded); i++) {
        check_word_unsupported(sgi_kona_doorbell_selectors,
                                sgi_kona_doorbell_num_selectors,
                                SGI_KONA_PORT_DOORBELL, excluded[i]);
    }
}

/*
 * The command-page tokens and the word count after each, first argument
 * included, as the kernel and the X server send them. 0x12e and 0x12f count
 * their start and entry count only, 0x130 its slot word only, 0x13d its
 * entry, width and height only, and 0x1a6 its command number only: the
 * device frames the colour-map and gamma entries, the cursor data, the
 * stipple rows and the video command's data words itself.
 */
static const SGICommandPortSelector expected_command_tokens[] = {
    { 0x1f1, 3 }, { 0x1f2, 3 }, { 0x1fd, 3 }, { 0x1f8, 1 }, { 0x1dd, 1 },
    { 0x1e9, 1 }, { 0x1e6, 3 }, { 0x128, 3 }, { 0x129, 13 }, { 0x12a, 4 },
    { 0x12b, 5 }, { 0x12e, 2 }, { 0x12f, 2 }, { 0x130, 1 }, { 0x132, 2 },
    { 0x133, 1 }, { 0x136, 7 }, { 0x139, 2 }, { 0x13a, 1 }, { 0x154, 2 },
    { 0x155, 12 }, { 0x1ff, 1 }, { 0x1a6, 1 }, { 0x142, 2 }, { 0x144, 4 },
    { 0x147, 6 }, { 0x149, 8 }, { 0x14b, 10 }, { 0x145, 4 }, { 0x152, 2 },
    { 0x153, 12 }, { 0x137, 4 }, { 0x12d, 4 }, { 0x13c, 1 }, { 0x13d, 3 },
    { 0x13e, 3 }, { 0x156, 3 }, { 0x157, 5 }, { 0x158, 13 }, { 0x1e4, 4 },
    { 0x1e1, 4 }, { 0x1e3, 8 }, { 0x172, 1 }, { 0x1fc, 1 }, { 0x1f9, 4 },
    { 0x1f3, 11 }, { 0x1ee, 3 }, { 0x1f4, 2 }, { 0x1ed, 3 },
    { 0x1e7, 1 }, { 0x1eb, 6 }, { 0x1c8, 2 },
    { 0x16d, 2 }, { 0x16e, 2 }, { 0x170, 1 }, { 0x171, 1 }, { 0x167, 4 },
    { 0x16f, 3 },
};

/*
 * The shipped table holds exactly the listed tokens with their counts,
 * with no duplicate, and none of them is the textport's sync word 1 or a
 * textport selector: the command page carries both, and an overlap the
 * sync word did not separate would route words to the wrong framer
 * without a word logged.
 */
static void test_command_table_shape(void)
{
    check_table_shape(sgi_kona_command_tokens, sgi_kona_command_num_tokens);
    check_table_matches(sgi_kona_command_tokens, sgi_kona_command_num_tokens,
                        expected_command_tokens,
                        ARRAY_SIZE(expected_command_tokens));
    for (unsigned i = 0; i < sgi_kona_command_num_tokens; i++) {
        g_assert_cmphex(sgi_kona_command_tokens[i].selector, !=, 1);
        for (unsigned j = 0; j < sgi_kona_textport_num_selectors; j++) {
            g_assert_cmphex(sgi_kona_command_tokens[i].selector, !=,
                            sgi_kona_textport_selectors[j].selector);
        }
    }
}

/*
 * The X tokens the header excludes: the data tokens whose counts the
 * grammar infers only from their names.
 */
static void test_command_excludes_uncounted_x_tokens(void)
{
    static const uint32_t excluded[] = {
        0x141, 0x143, 0x146, 0x148, 0x14a,
    };

    for (size_t i = 0; i < ARRAY_SIZE(excluded); i++) {
        check_word_unsupported(sgi_kona_command_tokens,
                               sgi_kona_command_num_tokens,
                               SGI_KONA_PORT_TEXTPORT, excluded[i]);
    }
}

static void test_command_tokens_frame(void)
{
    check_selector_frames(sgi_kona_command_tokens,
                          sgi_kona_command_num_tokens,
                          SGI_KONA_PORT_TEXTPORT);
}

/*
 * The GL client's commands and the words after each selector word, a
 * double counting two, as the senders write them: written out again here
 * rather than read from the model. The texture table, polygon stipple,
 * pop-lights and texel load count only the words before their data runs,
 * which the device consumes itself.
 */
static const SGICommandPortSelector expected_gl_tokens[] = {
    /* Buffer configuration, pack, read configuration and drawable. */
    { 0x1, 10 }, { 0x2, 1 }, { 0x4, 10 }, { 0x114, 1 },
    /* Make-current. */
    { 0x177, 1 }, { 0x1d7, 4 }, { 0x17e, 1 }, { 0xee, 1 }, { 0x1ce, 1 },
    { 0xe3, 4 }, { 0xc, 4 }, { 0xe4, 1 },
    /* Clears, begins, end, rect, flushes, finish, errors, resolve. */
    { 0x1d, 4 }, { 0x1a, 1 }, { 0x1b, 4 }, { 0x1c, 1 }, { 0x1e, 1 },
    { 0x1f, 2 }, { 0x57, 1 }, { 0x58, 1 }, { 0x59, 1 }, { 0x5a, 1 },
    { 0x5b, 1 }, { 0x5c, 1 }, { 0x5d, 1 }, { 0x5e, 1 }, { 0x5f, 1 },
    { 0x60, 1 }, { 0x71, 1 }, { 0x3, 1 }, { 0xc7, 1 }, { 0xc6, 1 },
    { 0x1cc, 1 }, { 0x3e, 1 }, { 0x1ca, 1 }, { 0x1d6, 1 },
    /* Matrices. */
    { 0xcf, 4 }, { 0xd0, 1 }, { 0xd1, 12 }, { 0xd2, 1 }, { 0xd3, 16 },
    { 0xd4, 32 }, { 0xd5, 16 }, { 0xd6, 32 }, { 0xd7, 12 }, { 0xd8, 1 },
    { 0xd9, 1 }, { 0xda, 8 }, { 0xdb, 4 }, { 0xdc, 6 }, { 0xdd, 3 },
    { 0xde, 6 }, { 0xdf, 3 },
    /* Colours, normals, texture coordinates, vertices, raster position. */
    { 0x61, 1 }, { 0x62, 6 }, { 0x63, 3 }, { 0x64, 3 }, { 0x66, 1 },
    { 0x67, 3 }, { 0x6a, 8 }, { 0x6b, 4 }, { 0x6c, 4 }, { 0x6d, 2 },
    { 0x6e, 1 }, { 0x6f, 4 }, { 0x76, 1 }, { 0x77, 6 }, { 0x78, 3 },
    { 0x79, 3 }, { 0x7a, 1 }, { 0x7c, 1 }, { 0x80, 2 }, { 0x82, 1 },
    { 0x84, 3 }, { 0x88, 4 }, { 0x8b, 4 }, { 0x8c, 2 }, { 0x8d, 2 },
    { 0x8e, 1 }, { 0x8f, 6 }, { 0x90, 3 }, { 0x91, 3 }, { 0x92, 1 },
    { 0x93, 8 }, { 0x94, 4 }, { 0x95, 4 }, { 0x96, 2 }, { 0xa7, 1 },
    { 0xa9, 2 }, { 0xad, 3 }, { 0xb1, 4 },
    /* Lighting. */
    { 0x7, 6 }, { 0x8, 6 }, { 0x9, 5 }, { 0xa, 5 }, { 0x97, 6 },
    { 0xb5, 2 },
    /* Enables and raster state. */
    { 0x24, 1 }, { 0x25, 1 }, { 0xc0, 1 }, { 0xce, 1 }, { 0x22, 1 },
    { 0xcb, 2 }, { 0xe6, 4 }, { 0xe7, 1 }, { 0xca, 2 }, { 0xcc, 3 },
    { 0xcd, 3 }, { 0x20, 1 }, { 0x21, 4 }, { 0x23, 1 }, { 0x2e, 1 },
    { 0xb6, 1 }, { 0xba, 1 }, { 0xbf, 2 }, { 0x168, 2 }, { 0x11b, 2 },
    { 0xb, 3 }, { 0xbd, 1 }, { 0xbe, 1 }, { 0xbc, 2 }, { 0xb7, 5 },
    { 0xb8, 5 }, { 0xb4, 9 }, { 0xbb, 2 }, { 0xc4, 1 }, { 0x37, 1 },
    { 0x33, 2 }, { 0x11, 5 }, { 0x12, 5 },
    /* Pixels, queries and the attribute stack. */
    { 0x3a, 8 }, { 0x39, 7 }, { 0x4c, 1 }, { 0xc9, 1 }, { 0xc8, 1 },
    { 0x119, 1 },
    /* Textures. */
    { 0x174, 20 }, { 0x176, 1 }, { 0x178, 1 }, { 0x179, 22 },
    { 0x1d5, 5 }, { 0x106, 5 }, { 0xed, 4 },
};

/*
 * The GL table holds exactly those commands, each a 16-bit selector, and
 * none of them is also a command-page token, or a word would have two
 * framers. The selectors whose counts are unknown stay out.
 */
static void test_gl_table_shape(void)
{
    static const uint32_t uncounted[] = {
        0x65, 0x68, 0x69, 0x70, 0xa8, 0xaa, 0xab, 0xac, 0xae, 0xaf, 0xb0,
        0xb2, 0xb3,
    };

    check_table_shape(sgi_kona_gl_tokens, sgi_kona_gl_num_tokens);
    check_table_matches(sgi_kona_gl_tokens, sgi_kona_gl_num_tokens,
                        expected_gl_tokens, ARRAY_SIZE(expected_gl_tokens));
    for (unsigned i = 0; i < sgi_kona_gl_num_tokens; i++) {
        g_assert_cmphex(sgi_kona_gl_tokens[i].selector, <=, 0xffff);
        for (unsigned j = 0; j < sgi_kona_command_num_tokens; j++) {
            g_assert_cmphex(sgi_kona_gl_tokens[i].selector, !=,
                            sgi_kona_command_tokens[j].selector);
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(uncounted); i++) {
        check_word_unsupported(sgi_kona_gl_tokens, sgi_kona_gl_num_tokens,
                               SGI_KONA_PORT_DOORBELL, uncounted[i]);
    }
    check_selector_frames(sgi_kona_gl_tokens, sgi_kona_gl_num_tokens,
                          SGI_KONA_PORT_DOORBELL);
}

/*
 * The video command table, written out again here rather than read from the
 * model: command number, data words at the senders (U where no sender was
 * found), reply id (NR for none) and reply data bytes.
 */
#define U  UINT32_MAX
#define NR UINT32_MAX
static const struct {
    uint32_t command, data_words, reply_id, reply_bytes;
} expected_video_commands[] = {
    { 0x00, U, 0, 4 }, { 0x01, U, NR, 0 }, { 0x02, U, 1, 4 },
    { 0x03, U, NR, 0 }, { 0x04, U, NR, 0 }, { 0x05, U, NR, 0 },
    { 0x06, U, NR, 0 }, { 0x07, U, NR, 0 }, { 0x08, 3, NR, 0 },
    { 0x09, 1, NR, 0 }, { 0x0a, U, 2, 4 }, { 0x0b, 2, NR, 0 },
    { 0x0c, 1, 3, 4 }, { 0x0d, 1, NR, 0 }, { 0x0e, 1, 4, 4 },
    { 0x0f, 2, NR, 0 }, { 0x10, 1, 5, 4 }, { 0x11, U, NR, 0 },
    { 0x12, 1, 6, 20 }, { 0x13, 1, 7, 76 }, { 0x14, 8, NR, 0 },
    { 0x15, 1, 8, 72 }, { 0x16, 383, NR, 0 }, { 0x17, 3, NR, 0 },
    { 0x18, 1, NR, 0 }, { 0x19, 16, NR, 0 }, { 0x1a, 1, NR, 0 },
    { 0x1b, U, NR, 0 }, { 0x1c, U, NR, 0 }, { 0x1d, U, NR, 0 },
    { 0x1e, 2, NR, 0 }, { 0x1f, 2, NR, 0 }, { 0x20, 1, 9, 4 },
    { 0x21, 2, NR, 0 }, { 0x22, 1, 10, 4 }, { 0x23, 4, NR, 0 },
    { 0x24, 1, 11, 24 }, { 0x25, 2, NR, 0 }, { 0x26, 1, 12, 4 },
    { 0x27, U, NR, 0 }, { 0x28, U, NR, 0 }, { 0x29, U, NR, 0 },
    { 0x2a, U, NR, 0 }, { 0x2b, 1, NR, 0 }, { 0x2c, U, 13, 4 },
    { 0x2d, U, NR, 0 }, { 0x2e, 1, NR, 0 }, { 0x2f, U, NR, 0 },
    { 0x30, U, 14, 4 }, { 0x31, 1, NR, 0 }, { 0x32, 1, 15, 4 },
    { 0x33, 1, NR, 0 }, { 0x34, 2, NR, 0 }, { 0x35, U, NR, 0 },
    { 0x36, U, NR, 0 }, { 0x37, U, NR, 0 }, { 0x38, 263, NR, 0 },
    { 0x39, U, NR, 0 }, { 0x3a, 1, 16, 24 }, { 0x3b, 1, NR, 0 },
    { 0x3c, 1, NR, 0 }, { 0x3d, 1, 17, 12 }, { 0x3e, 1, NR, 0 },
    { 0x3f, U, 18, 4 }, { 0x40, 3, NR, 0 }, { 0x41, U, NR, 0 },
    { 0x42, 1, 19, 8 }, { 0x43, 9, NR, 0 }, { 0x44, 1, NR, 0 },
    { 0x45, 1, 20, 4 }, { 0x46, 2, NR, 0 }, { 0x47, U, 21, 4 },
    { 0x48, 1, NR, 0 }, { 0x49, U, 22, 4 }, { 0x4a, 1, 23, 20 },
    { 0x4b, 5, NR, 0 }, { 0x4c, 1, 24, 16 }, { 0x4d, 1, NR, 0 },
    { 0x4e, U, NR, 0 }, { 0x4f, 7, NR, 0 }, { 0x50, 1, 25, 68 },
    { 0x51, U, 26, 136 }, { 0x52, 6, 27, 4 }, { 0x53, 1, NR, 0 },
    { 0x54, 1, NR, 0 }, { 0x55, 1, 28, 4 }, { 0x56, 3, NR, 0 },
    { 0x57, U, 29, 8 }, { 0x58, 3, NR, 0 }, { 0x59, U, 30, 8 },
    { 0x5a, 1, NR, 0 }, { 0x5b, 42, NR, 0 }, { 0x5c, U, 31, 164 },
};
#undef U
#undef NR

/*
 * The video command table holds all 93 commands, entry c being command c,
 * each with the grammar's data length, reply id and reply size, and so no
 * number twice. Every reply id is one the X server's reply wait accepts,
 * 0 to 0x1f, each is used by exactly one command, and each command's
 * reply size is the reply-size table's entry for its id; every data
 * length fits what the device's framing holds.
 */
static void test_video_table_shape(void)
{
    bool id_used[SGI_KONA_VIDEO_REPLY_IDS] = { false };

    g_assert_cmpuint(sgi_kona_video_num_commands, ==, 93);
    g_assert_cmpuint(sgi_kona_video_num_commands, ==,
                     ARRAY_SIZE(expected_video_commands));
    g_assert_cmpuint(SGI_KONA_VIDEO_COMMANDS, ==, 93);
    for (unsigned i = 0; i < sgi_kona_video_num_commands; i++) {
        const SGIKonaVideoCommand *v = &sgi_kona_video_commands[i];

        g_assert_cmphex(v->command, ==, i);
        g_assert_cmphex(v->command, ==, expected_video_commands[i].command);
        g_assert_nonnull(v->name);
        g_assert_cmpuint(v->data_words, ==,
                         expected_video_commands[i].data_words);
        g_assert_cmpuint(v->reply_id, ==, expected_video_commands[i].reply_id);
        g_assert_cmpuint(v->reply_bytes, ==,
                         expected_video_commands[i].reply_bytes);
        if (v->data_words != SGI_KONA_VIDEO_DATA_UNKNOWN) {
            g_assert_cmpuint(v->data_words, >=, 1);
            g_assert_cmpuint(v->data_words, <=,
                             SGI_KONA_VIDEO_MAX_DATA_WORDS);
        }
        if (v->reply_id == SGI_KONA_VIDEO_NO_REPLY) {
            g_assert_cmpuint(v->reply_bytes, ==, 0);
            continue;
        }
        g_assert_cmpuint(v->reply_id, <, SGI_KONA_VIDEO_REPLY_IDS);
        g_assert_false(id_used[v->reply_id]);
        id_used[v->reply_id] = true;
        g_assert_cmpuint(v->reply_bytes, ==,
                         sgi_kona_video_reply_sizes[v->reply_id]);
        g_assert_cmpuint(v->reply_bytes % 4, ==, 0);
        g_assert_cmpuint(v->reply_bytes, <=, SGI_KONA_VIDEO_MAX_REPLY_BYTES);
    }
    for (unsigned id = 0; id < SGI_KONA_VIDEO_REPLY_IDS; id++) {
        g_assert_true(id_used[id]);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-kona-ports/fifo/table-shape", test_fifo_table_shape);
    g_test_add_func("/sgi-kona-ports/command/table-shape",
                    test_command_table_shape);
    g_test_add_func("/sgi-kona-ports/command/tokens-frame",
                    test_command_tokens_frame);
    g_test_add_func("/sgi-kona-ports/command/excludes-uncounted-x-tokens",
                    test_command_excludes_uncounted_x_tokens);
    g_test_add_func("/sgi-kona-ports/gl/table-shape", test_gl_table_shape);
    g_test_add_func("/sgi-kona-ports/video/table-shape",
                    test_video_table_shape);
    g_test_add_func("/sgi-kona-ports/fifo/selectors-frame",
                    test_fifo_selectors_frame);
    g_test_add_func("/sgi-kona-ports/fifo/excluded-selectors",
                    test_fifo_excluded_selectors);
    g_test_add_func("/sgi-kona-ports/textport/table-shape",
                    test_textport_table_shape);
    g_test_add_func("/sgi-kona-ports/textport/selectors-frame",
                    test_textport_selectors_frame);
    g_test_add_func("/sgi-kona-ports/textport/excludes-bulk-transfer",
                    test_textport_excludes_bulk_transfer);
    g_test_add_func("/sgi-kona-ports/doorbell/table-shape",
                    test_doorbell_table_shape);
    g_test_add_func("/sgi-kona-ports/doorbell/selectors-frame",
                    test_doorbell_selectors_frame);
    g_test_add_func("/sgi-kona-ports/doorbell/excluded-selectors",
                    test_doorbell_excluded_selectors);
    return g_test_run();
}
