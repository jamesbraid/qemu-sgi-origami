/* SPDX-License-Identifier: GPL-2.0-or-later */
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

static void test_fifo_table_shape(void)
{
    check_table_shape(sgi_kona_fifo_selectors, sgi_kona_fifo_num_selectors);
}

static void test_fifo_selectors_frame(void)
{
    check_selector_frames(sgi_kona_fifo_selectors,
                           sgi_kona_fifo_num_selectors, SGI_KONA_PORT_FIFO);
}

/*
 * Selectors the header's comment excludes for a stated reason:
 * contradictory argc (0x1f3, 0x172), selector-last framing (0x1fd),
 * dual-port ambiguity (0x100), unstated argc (0x1eb), and unconfirmed
 * word order (0x1ee, 0x1f9, 0x1f2). None may appear in the shipped
 * table.
 */
static void test_fifo_excluded_selectors(void)
{
    static const uint32_t excluded[] = {
        0x1fd, 0x1f3, 0x172, 0x100, 0x1eb, 0x1ee, 0x1f9, 0x1f2,
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
 * glBegin, glClear, glVertex2f/3f, glReadPixels and one of
 * __glkonResolveContext's three call sites: excluded for no stated argc.
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-kona-ports/fifo/table-shape", test_fifo_table_shape);
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
