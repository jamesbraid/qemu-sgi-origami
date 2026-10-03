/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA DMA engine qtests.
 *
 * The engine (hw/misc/sgi-kona-dma.c) frames kona_idma's packet, 0x1eb and
 * six words, and kona_dmawrite's 0x1c8 record on the kernel FIFO port, and
 * moves each packet's bytes through the shared pixel DMA. These cases pin
 * where a transfer's bytes land (the framebuffer at x-test-dma-fb-offset,
 * or the list plane at a record's pipe-memory address), which transfers
 * are refused and by whom, and the completion each packet publishes: one
 * record carrying the cookie its word 6 asks for. include/hw/misc/
 * sgi-kona-dma.h has the packet and what is approximated.
 *
 * The words go through qtest_writel() one at a time at the push register,
 * with the kick literals the driver writes around them, rather than
 * through a helper that would hide the framing.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define SMALL_WINDOW(widget) \
    (SPACE_IO + (uint64_t)(widget) * SMALL_WINDOW_SIZE)

#define KONA_WIDGET 11
#define KONA_BASE   SMALL_WINDOW(KONA_WIDGET)
#define KONA_REG(offset) (KONA_BASE + (offset))

/*
 * The push register is an approximation (include/hw/misc/sgi-kona-dma.h);
 * the kick register is established.
 */
#define REG_DMA_FIFO_PUSH KONA_REG(0x80007c)
#define REG_DMA_FIFO_KICK KONA_REG(0xc00108)
#define REG_COMMAND       KONA_REG(0xc10000)
#define REG_INT_CAUSE     KONA_REG(0x800238)
#define REG_MAILBOX       KONA_REG(0x800240)
#define CAUSE_MAILBOX     0x10u
#define CAUSE_FATAL       0x100u

/*
 * fabric-hub-widget=9 targets node 0's own Hub (tests/qtest/
 * sn-builder-test.c's own HUB_WIDGET_ON_XBOW precedent); see the struct
 * field comment on SGIKonaState's fabric_hub_widget for the approximation
 * this stands in for.
 */
#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

/*
 * origin2000's default topology is the full four-node/multi-CPU module,
 * too large to migrate twice inside a single qtest's time budget --
 * sn0-origin2000-kona-test.c's own MIGRATION_ARGS pins the same machine
 * type down for the same reason.
 */
#define MIGRATION_ARGS MACHINE_ARGS

#define KONA_QOM_PATH "/machine/module0/kona0"

/*
 * The model's sizes, repeated as literals rather than included: a test
 * that borrowed the model's own constant could not catch the model
 * getting it wrong. The list plane's rows are 4 KiB and it is 1 MiB by
 * default.
 */
#define KONA_FB_WIDTH   1280
#define KONA_FB_HEIGHT  1024
#define KONA_LIST_ROW   4096
#define KONA_LIST_BYTES (1024 * 1024)

/* Plane ids in the order the model declares them. */
#define PLANE_FRONT     0
#define PLANE_FRONT_TAG 2
#define PLANE_LIST      4
#define TAG_DIRECT      0

#define KONA_DMA_PACKET     0x1ebu
#define KONA_DMA_LIST_WRITE 0x1c8u
#define KONA_DMA_ADDR_TAG   0x9000u
#define KONA_DMA_DONE_WORD  0x100u

/* SGICompletionKind and SGIFailure (include/hw/display/sgi/head-ops.h). */
#define DONE_WRITTEN 2
#define DONE_FAILED  16
#define FAIL_BUS     1
#define FAIL_BOUNDS  2

/* The completion word 6 = 0x100 asks for: mailbox 0x100 under cause 0x10. */
#define COOKIE_MAILBOX_DONE ((UINT64_C(0x10) << 32) | KONA_DMA_DONE_WORD)

/*
 * Guest-visible fabric addresses in qtest are composed as
 * ((uint64_t)NODE << 32) | offset (tests/qtest/sn0-origin2000-mgras-test.c's
 * own dma_xfer_node_addr() precedent); node 0's low offsets are directly
 * qtest_memwrite()/qtest_memread()-reachable guest RAM. Each case uses its
 * own offsets.
 */
static uint64_t dma_node_addr(uint32_t offset)
{
    return offset;
}

static QTestState *qts;

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

static void kona_qom_set_uint(QTestState *s, const char *property,
                              uint64_t value)
{
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': %s, 'value': %" PRIu64 "}}", KONA_QOM_PATH, property,
        value);
}

/* A word of any plane, in plane coordinates. */
static uint32_t kona_plane_word(QTestState *s, uint32_t plane, uint32_t x,
                                uint32_t y)
{
    kona_qom_set_uint(s, "x-test-pixel-x", x);
    kona_qom_set_uint(s, "x-test-pixel-y", y);
    kona_qom_set_uint(s, "x-test-plane", plane);
    return kona_qom_get_uint(s, "x-test-plane-word");
}

/*
 * The front plane's word at display line `line` (0 at the top), column x:
 * the plane is bottom-origin.
 */
static uint32_t kona_fb_word(QTestState *s, uint32_t x, uint32_t line)
{
    return kona_plane_word(s, PLANE_FRONT, x, KONA_FB_HEIGHT - 1 - line);
}

/* The list plane's word at pipe-memory byte address `at`. */
static uint32_t kona_list_word(QTestState *s, uint32_t at)
{
    return kona_plane_word(s, PLANE_LIST, (at % KONA_LIST_ROW) / 4,
                           at / KONA_LIST_ROW);
}

static void kona_set_dma_fb_offset(QTestState *s, uint32_t offset)
{
    kona_qom_set_uint(s, "x-test-dma-fb-offset", offset);
}

static void kona_set_dma_reverse(QTestState *s, bool reverse)
{
    qtest_qom_set_bool(s, KONA_QOM_PATH, "x-test-dma-reverse", reverse);
}

static void kona_reset(void)
{
    qtest_system_reset(qts);
    kona_set_dma_fb_offset(qts, 0);
    kona_set_dma_reverse(qts, false);
}

static void kona_dma_push_word(QTestState *s, uint32_t word)
{
    qtest_writel(s, REG_DMA_FIFO_PUSH, word);
}

/*
 * A whole packet from its raw words, bracketed by the kick literals
 * kona_idma writes: 0x30000 before the selector, 0x10000 before word 1
 * and 0 after word 6.
 */
static void kona_dma_packet_raw(QTestState *s, const uint32_t w[6])
{
    qtest_writel(s, REG_DMA_FIFO_KICK, 0x30000);
    kona_dma_push_word(s, KONA_DMA_PACKET);
    qtest_writel(s, REG_DMA_FIFO_KICK, 0x10000);
    for (unsigned i = 0; i < 6; i++) {
        kona_dma_push_word(s, w[i]);
    }
    qtest_writel(s, REG_DMA_FIFO_KICK, 0);
}

/* A well-formed packet: buffer at addr, lines of per_line bytes. */
static void kona_dma_packet(QTestState *s, uint64_t addr, uint32_t total,
                            uint32_t per_line, uint32_t gap, uint32_t done)
{
    kona_dma_packet_raw(s, (const uint32_t[6]) {
        (uint32_t)(addr >> 12),
        ((uint32_t)addr & 0xfffu) | KONA_DMA_ADDR_TAG,
        total, per_line, gap, done,
    });
}

/* kona_dmawrite's record: the next packet's bytes go to pipe memory. */
static void kona_dma_list_write(QTestState *s, uint32_t dest, uint32_t bytes)
{
    kona_dma_push_word(s, KONA_DMA_LIST_WRITE);
    kona_dma_push_word(s, dest);
    kona_dma_push_word(s, bytes);
}

static void kona_write_be32_array(QTestState *s, uint64_t addr,
                                  const uint32_t *values, unsigned count)
{
    g_autofree uint8_t *buf = g_malloc(count * 4);

    for (unsigned i = 0; i < count; i++) {
        stl_be_p(buf + i * 4, values[i]);
    }
    qtest_memwrite(s, addr, buf, count * 4);
}

static void kona_assert_be32_array(QTestState *s, uint64_t addr,
                                   const uint32_t *expected, unsigned count)
{
    g_autofree uint8_t *buf = g_malloc(count * 4);

    qtest_memread(s, addr, buf, count * 4);
    for (unsigned i = 0; i < count; i++) {
        /* ldl_be_p() is signed: mask so bit 31 does not sign-extend. */
        g_assert_cmphex((uint32_t)ldl_be_p(buf + i * 4), ==, expected[i]);
    }
}

static uint32_t kona_completions(QTestState *s)
{
    return kona_qom_get_uint(s, "x-test-dma-completions");
}

/* The engine's last completion is kind, carrying cookie and payload. */
static void kona_assert_last_completion(QTestState *s, uint32_t kind,
                                        uint64_t cookie, uint64_t payload)
{
    g_assert_cmpuint(kona_qom_get_uint(s, "x-test-dma-last-kind"), ==, kind);
    g_assert_cmphex(kona_qom_get_uint(s, "x-test-dma-last-cookie"), ==,
                    cookie);
    g_assert_cmphex(kona_qom_get_uint(s, "x-test-dma-last-payload"), ==,
                    payload);
}

static const uint32_t dma_pixels[4] = {
    0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00,
};

#define DMA_FWD_SRC UINT64_C(0x9004)

/*
 * One line of four pixels lands at the top display line's left end, each
 * word whole with the direct tag, and the packet completes once, WRITTEN
 * with its 16 bytes and the mailbox cookie its word 6 asks for. The
 * source is not page-aligned (low 12 bits 0x004), so words 1 and 2's
 * split is what places it.
 */
static void test_dma_forward_transfer_writes_framebuffer(void)
{
    kona_reset();
    kona_write_be32_array(qts, dma_node_addr(DMA_FWD_SRC), dma_pixels, 4);

    kona_dma_packet(qts, dma_node_addr(DMA_FWD_SRC), 16, 16, 0,
                    KONA_DMA_DONE_WORD);

    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(kona_fb_word(qts, i, 0), ==, dma_pixels[i]);
    }
    g_assert_cmphex(kona_plane_word(qts, PLANE_FRONT_TAG, 0,
                                    KONA_FB_HEIGHT - 1), ==, TAG_DIRECT);
    g_assert_cmpuint(kona_completions(qts), ==, 1);
    kona_assert_last_completion(qts, DONE_WRITTEN, COOKIE_MAILBOX_DONE, 16);
}

#define DMA_LINES_SRC UINT64_C(0x13ff8)

/*
 * The descriptor is two-dimensional: two lines of two pixels with an
 * eight-byte gap after each. Line 0 lands on display line 1 from column
 * 5 (fb offset 1285) and line 1 on the line below it; the gap's words
 * land nowhere. The source straddles a page boundary, so the transfer
 * reads two pages of its page list.
 */
static void test_dma_lines_and_gap_place_rows(void)
{
    static const uint32_t src[6] = {
        0x0a0b0c0d, 0x01020304, 0xeeeeeeee, 0xeeeeeeee,
        0x05060708, 0x090a0b0c,
    };

    kona_reset();
    kona_write_be32_array(qts, dma_node_addr(DMA_LINES_SRC), src, 6);
    kona_set_dma_fb_offset(qts, KONA_FB_WIDTH + 5);

    kona_dma_packet(qts, dma_node_addr(DMA_LINES_SRC), 16, 8, 8,
                    KONA_DMA_DONE_WORD);

    g_assert_cmphex(kona_fb_word(qts, 5, 1), ==, src[0]);
    g_assert_cmphex(kona_fb_word(qts, 6, 1), ==, src[1]);
    g_assert_cmphex(kona_fb_word(qts, 7, 1), ==, 0);
    g_assert_cmphex(kona_fb_word(qts, 5, 2), ==, src[4]);
    g_assert_cmphex(kona_fb_word(qts, 6, 2), ==, src[5]);
    g_assert_cmphex(kona_fb_word(qts, 5, 0), ==, 0);
    /* One completion per packet: the first line's carries no cookie. */
    kona_assert_last_completion(qts, DONE_WRITTEN, COOKIE_MAILBOX_DONE, 8);
}

#define DMA_REV_DST UINT64_C(0xa004)

/*
 * Framebuffer to guest, the qtest-only reverse direction: pixels written
 * by a forward packet come back to a second, zeroed buffer.
 */
static void test_dma_reverse_transfer_reads_framebuffer_to_guest(void)
{
    static const uint32_t zero[4];

    kona_reset();
    kona_write_be32_array(qts, dma_node_addr(DMA_FWD_SRC), dma_pixels, 4);
    kona_dma_packet(qts, dma_node_addr(DMA_FWD_SRC), 16, 16, 0,
                    KONA_DMA_DONE_WORD);

    kona_write_be32_array(qts, dma_node_addr(DMA_REV_DST), zero, 4);
    kona_set_dma_reverse(qts, true);
    kona_dma_packet(qts, dma_node_addr(DMA_REV_DST), 16, 16, 0,
                    KONA_DMA_DONE_WORD);

    kona_assert_be32_array(qts, dma_node_addr(DMA_REV_DST), dma_pixels, 4);
    kona_assert_last_completion(qts, DONE_WRITTEN, COOKIE_MAILBOX_DONE, 16);
}

#define DMA_COOKIE_SRC UINT64_C(0x14004)

/*
 * Word 6 decides the completion: 0x100 carries the mailbox cookie, 0 a
 * cookie that posts nothing. Both packets still transfer.
 */
static void test_dma_completion_cookie_follows_word_6(void)
{
    kona_reset();
    kona_write_be32_array(qts, dma_node_addr(DMA_COOKIE_SRC), dma_pixels, 4);

    kona_dma_packet(qts, dma_node_addr(DMA_COOKIE_SRC), 4, 4, 0, 0);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, dma_pixels[0]);
    kona_assert_last_completion(qts, DONE_WRITTEN, 0, 4);

    kona_dma_packet(qts, dma_node_addr(DMA_COOKIE_SRC), 8, 8, 0,
                    KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, 1, 0), ==, dma_pixels[1]);
    kona_assert_last_completion(qts, DONE_WRITTEN, COOKIE_MAILBOX_DONE, 8);
    g_assert_cmpuint(kona_completions(qts), ==, 2);
}

#define DMA_TAG_SRC UINT64_C(0xb004)

/*
 * Word 2 must carry the fixed 0x9000 tag: a packet whose word 2 carries
 * another is refused, FAILED with BUS and the packet's cookie, and moves
 * nothing. The port stays in step for the next packet.
 */
static void test_dma_tag_mismatch_refused(void)
{
    uint64_t addr = dma_node_addr(DMA_TAG_SRC);

    kona_reset();
    kona_write_be32_array(qts, addr, dma_pixels, 4);

    kona_dma_packet_raw(qts, (const uint32_t[6]) {
        (uint32_t)(addr >> 12), ((uint32_t)addr & 0xfffu) | 0xa000u,
        16, 16, 0, KONA_DMA_DONE_WORD,
    });
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, 0);
    kona_assert_last_completion(qts, DONE_FAILED, COOKIE_MAILBOX_DONE,
                                FAIL_BUS);

    kona_dma_packet(qts, addr, 16, 16, 0, KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, dma_pixels[0]);
}

#define DMA_WIDE_SRC UINT64_C(0x20000)

/*
 * A line of 1281 pixels is wider than the front plane: the shared pixel
 * DMA refuses it at map time, before reading the guest, and the packet
 * completes FAILED with BOUNDS. Nothing lands, not even the pixels that
 * would have fitted.
 */
static void test_dma_line_wider_than_plane_refused(void)
{
    uint32_t bytes = (KONA_FB_WIDTH + 1) * 4;
    g_autofree uint32_t *src = g_new(uint32_t, KONA_FB_WIDTH + 1);

    for (unsigned i = 0; i <= KONA_FB_WIDTH; i++) {
        src[i] = 0x01000000 | i;
    }
    kona_reset();
    kona_write_be32_array(qts, dma_node_addr(DMA_WIDE_SRC), src,
                          KONA_FB_WIDTH + 1);

    kona_dma_packet(qts, dma_node_addr(DMA_WIDE_SRC), bytes, bytes, 0,
                    KONA_DMA_DONE_WORD);

    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, 0);
    g_assert_cmphex(kona_fb_word(qts, KONA_FB_WIDTH - 1, 0), ==, 0);
    g_assert_cmpuint(kona_completions(qts), ==, 1);
    kona_assert_last_completion(qts, DONE_FAILED, COOKIE_MAILBOX_DONE,
                                FAIL_BOUNDS);
}

#define DMA_SHAPE_SRC UINT64_C(0xd004)

/*
 * A descriptor that is not whole lines of whole words is refused, FAILED
 * with BOUNDS: a zero per-line count, a per-line count that is not whole
 * words, and a total that is not a whole number of lines.
 */
static void test_dma_shape_constraints_refused(void)
{
    static const uint32_t shapes[][2] = {
        { 16, 0 }, { 6, 6 }, { 16, 12 }, { 0, 4 },
    };
    uint64_t addr = dma_node_addr(DMA_SHAPE_SRC);

    kona_reset();
    kona_write_be32_array(qts, addr, dma_pixels, 4);

    for (unsigned i = 0; i < ARRAY_SIZE(shapes); i++) {
        kona_dma_packet(qts, addr, shapes[i][0], shapes[i][1], 0,
                        KONA_DMA_DONE_WORD);
        g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, 0);
        kona_assert_last_completion(qts, DONE_FAILED, COOKIE_MAILBOX_DONE,
                                    FAIL_BOUNDS);
    }

    kona_dma_packet(qts, addr, 16, 16, 0, KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, dma_pixels[0]);
}

#define DMA_OFFSET_SRC UINT64_C(0xe004)

/*
 * A placement that pushes a line past the framebuffer's right edge, or
 * lines past its bottom, is refused, FAILED with BOUNDS: the model
 * refuses it, since the shared layer bounds a transfer by the plane's
 * size and would drop the overhang silently. The placement is the
 * model's own, so the refusal carries no cookie and leaves the fatal
 * cause bit clear, though word 6 asks for a completion: the driver kills
 * the pipe on that bit.
 */
static void test_dma_destination_offset_out_of_range_refused(void)
{
    uint32_t last_two = KONA_FB_WIDTH * KONA_FB_HEIGHT - 2;
    uint32_t last_line = KONA_FB_WIDTH * (KONA_FB_HEIGHT - 1);
    uint64_t addr = dma_node_addr(DMA_OFFSET_SRC);

    kona_reset();
    kona_write_be32_array(qts, addr, dma_pixels, 4);

    kona_set_dma_fb_offset(qts, last_two);
    kona_dma_packet(qts, addr, 16, 16, 0, KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, KONA_FB_WIDTH - 2,
                                 KONA_FB_HEIGHT - 1), ==, 0);
    g_assert_cmphex(kona_fb_word(qts, KONA_FB_WIDTH - 1,
                                 KONA_FB_HEIGHT - 1), ==, 0);
    kona_assert_last_completion(qts, DONE_FAILED, 0, FAIL_BOUNDS);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);

    kona_set_dma_fb_offset(qts, last_line);
    kona_dma_packet(qts, addr, 16, 8, 0, KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, 0, KONA_FB_HEIGHT - 1), ==, 0);
    kona_assert_last_completion(qts, DONE_FAILED, 0, FAIL_BOUNDS);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);

    kona_set_dma_fb_offset(qts, 0);
    kona_dma_packet(qts, addr, 16, 16, 0, KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, dma_pixels[0]);
}

#define DMA_LIST_SRC UINT64_C(0x30000)
#define DMA_LIST_DEST 0xffcu
#define DMA_LIST_WORDS (1 + KONA_LIST_ROW / 4 + 1)

/*
 * kona_dmawrite's list copy: a 0x1c8 record naming pipe-memory address
 * 0xffc and 4,104 bytes, then a packet of that many bytes in one line.
 * The bytes land in the list plane from that address, across the end of
 * row 0, the whole of row 1 and the start of row 2, and nothing lands in
 * the framebuffer. The packet completes once with its cookie, on the
 * last of its three items.
 */
static void test_dma_list_write_lands_in_list_plane(void)
{
    g_autofree uint32_t *src = g_new(uint32_t, DMA_LIST_WORDS);
    uint32_t bytes = DMA_LIST_WORDS * 4;

    for (unsigned i = 0; i < DMA_LIST_WORDS; i++) {
        src[i] = 0x16d00000 + i;
    }
    kona_reset();
    kona_write_be32_array(qts, dma_node_addr(DMA_LIST_SRC), src,
                          DMA_LIST_WORDS);

    kona_dma_list_write(qts, DMA_LIST_DEST, bytes);
    kona_dma_packet(qts, dma_node_addr(DMA_LIST_SRC), bytes, bytes, 0,
                    KONA_DMA_DONE_WORD);

    g_assert_cmphex(kona_list_word(qts, DMA_LIST_DEST - 4), ==, 0);
    g_assert_cmphex(kona_list_word(qts, DMA_LIST_DEST), ==, src[0]);
    g_assert_cmphex(kona_list_word(qts, KONA_LIST_ROW), ==, src[1]);
    g_assert_cmphex(kona_list_word(qts, 2 * KONA_LIST_ROW - 4), ==,
                    src[DMA_LIST_WORDS - 2]);
    g_assert_cmphex(kona_list_word(qts, 2 * KONA_LIST_ROW), ==,
                    src[DMA_LIST_WORDS - 1]);
    g_assert_cmphex(kona_list_word(qts, 2 * KONA_LIST_ROW + 4), ==, 0);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, 0);
    g_assert_cmpuint(kona_completions(qts), ==, 3);
    kona_assert_last_completion(qts, DONE_WRITTEN, COOKIE_MAILBOX_DONE, 4);

    /* The record is spent: the next packet goes to the framebuffer. */
    kona_dma_packet(qts, dma_node_addr(DMA_LIST_SRC), 4, 4, 0,
                    KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, src[0]);
}

/*
 * A list write that would run past the list plane's end, or whose record
 * does not match the packet's byte count, is refused, FAILED with BOUNDS,
 * and writes nothing.
 */
static void test_dma_list_write_outside_plane_refused(void)
{
    kona_reset();
    kona_write_be32_array(qts, dma_node_addr(DMA_LIST_SRC), dma_pixels, 4);

    kona_dma_list_write(qts, KONA_LIST_BYTES - 4, 8);
    kona_dma_packet(qts, dma_node_addr(DMA_LIST_SRC), 8, 8, 0,
                    KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_list_word(qts, KONA_LIST_BYTES - 4), ==, 0);
    kona_assert_last_completion(qts, DONE_FAILED, COOKIE_MAILBOX_DONE,
                                FAIL_BOUNDS);

    kona_dma_list_write(qts, 0, 8);
    kona_dma_packet(qts, dma_node_addr(DMA_LIST_SRC), 16, 16, 0,
                    KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_list_word(qts, 0), ==, 0);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, 0);
    kona_assert_last_completion(qts, DONE_FAILED, COOKIE_MAILBOX_DONE,
                                FAIL_BOUNDS);
}

#define DMA_STRAY_SRC UINT64_C(0x10004)

/*
 * Words that are not a selector the port frames are dropped one at a
 * time, and the kick's values frame nothing, so a packet after them
 * completes normally.
 */
static void test_dma_stray_words_resynchronize(void)
{
    uint64_t addr = dma_node_addr(DMA_STRAY_SRC);

    kona_reset();
    qtest_writel(qts, REG_DMA_FIFO_KICK, 6);
    kona_dma_push_word(qts, 0xdeadbeef);
    kona_dma_push_word(qts, 0x9);
    g_assert_cmpuint(kona_completions(qts), ==, 0);

    kona_write_be32_array(qts, addr, dma_pixels, 4);
    kona_dma_packet(qts, addr, 16, 16, 0, KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, dma_pixels[0]);
    g_assert_cmpuint(kona_completions(qts), ==, 1);
}

#define DMA_PARTIAL_SRC UINT64_C(0x11004)

/*
 * A packet pushed in two parts moves nothing and completes nothing until
 * its last word arrives.
 */
static void test_dma_partial_packet_holds_state_across_pushes(void)
{
    uint64_t addr = dma_node_addr(DMA_PARTIAL_SRC);

    kona_reset();
    kona_write_be32_array(qts, addr, dma_pixels, 4);

    kona_dma_push_word(qts, KONA_DMA_PACKET);
    kona_dma_push_word(qts, (uint32_t)(addr >> 12));
    kona_dma_push_word(qts, ((uint32_t)addr & 0xfffu) | KONA_DMA_ADDR_TAG);
    kona_dma_push_word(qts, 16);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, 0);
    g_assert_cmpuint(kona_completions(qts), ==, 0);

    kona_dma_push_word(qts, 16);
    kona_dma_push_word(qts, 0);
    kona_dma_push_word(qts, KONA_DMA_DONE_WORD);
    g_assert_cmphex(kona_fb_word(qts, 0, 0), ==, dma_pixels[0]);
    g_assert_cmphex(kona_fb_word(qts, 3, 0), ==, dma_pixels[3]);
    kona_assert_last_completion(qts, DONE_WRITTEN, COOKIE_MAILBOX_DONE, 16);
}

#define DMA_PAGE_SRC UINT64_C(0x15004)

/*
 * The traced guest run sends the kernel's packets on the +0xc10000
 * command page, so the record and the packet reach the engine there as
 * plain words too, token first, with the kick literals around the
 * packet.
 */
static void test_dma_command_page_reaches_engine(void)
{
    uint64_t addr = dma_node_addr(DMA_PAGE_SRC);
    static const uint32_t record[] = { KONA_DMA_LIST_WRITE, 0x40, 8 };
    uint32_t packet[] = {
        KONA_DMA_PACKET, (uint32_t)(addr >> 12),
        ((uint32_t)addr & 0xfffu) | KONA_DMA_ADDR_TAG, 8, 8, 0,
        KONA_DMA_DONE_WORD,
    };

    kona_reset();
    kona_write_be32_array(qts, addr, dma_pixels, 2);

    for (unsigned i = 0; i < ARRAY_SIZE(record); i++) {
        qtest_writel(qts, REG_COMMAND, record[i]);
    }
    qtest_writel(qts, REG_DMA_FIFO_KICK, 0x30000);
    qtest_writel(qts, REG_COMMAND, packet[0]);
    qtest_writel(qts, REG_DMA_FIFO_KICK, 0x10000);
    for (unsigned i = 1; i < ARRAY_SIZE(packet); i++) {
        qtest_writel(qts, REG_COMMAND, packet[i]);
    }
    qtest_writel(qts, REG_DMA_FIFO_KICK, 0);

    g_assert_cmphex(kona_list_word(qts, 0x40), ==, dma_pixels[0]);
    g_assert_cmphex(kona_list_word(qts, 0x44), ==, dma_pixels[1]);
    kona_assert_last_completion(qts, DONE_WRITTEN, COOKIE_MAILBOX_DONE, 8);
}

/*
 * The display-list packets, sent on the command page as the GL client's
 * doorbell words reach it. Their observable effect in these cases is the
 * list's own words: textport commands (sync word 1, then a command), so a
 * list that reaches the command page's decoder in order draws.
 */
#define LIST_CALL        0x16du
#define LIST_CALL_POOL   0x16eu
#define LIST_CALLS       0x170u
#define LIST_CALLS_POOL  0x171u
#define LIST_PULL        0x167u
#define LIST_MOVE        0x16fu
#define DMA_POOL         0x1edu
#define DONE_CONSUMED    1
#define FAIL_BUSY        5

/* The display-list class's capacity in words: 15 MiB. */
#define LIST_MAX_WORDS   (15u * 1024 * 1024 / 4)

static void kona_command(QTestState *s, const uint32_t *words, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        qtest_writel(s, REG_COMMAND, words[i]);
    }
}

/* kona_dmapool's registration: 0x1ed, the split address, the length. */
static void kona_register_pool(QTestState *s, uint64_t base, uint32_t len)
{
    kona_command(s, (const uint32_t[]) {
        DMA_POOL, (uint32_t)(base >> 12),
        ((uint32_t)base & 0xfffu) | KONA_DMA_ADDR_TAG, len,
    }, 4);
}

/*
 * A list that loads colour-map entry 5, selects it and draws a point at
 * (40, 60): eleven words the test splits across ranges.
 */
static const uint32_t list_words[] = {
    1, 0x3, 5, 0x11 << 2, 0x22 << 2, 0x33 << 2,
    1, 0x2, 5,
    1, 0x6, 40, 60,
};
#define LIST_POINT_X 40
#define LIST_POINT_Y 60
#define LIST_POINT_RGB 0x112233

static uint32_t kona_point_pixel(QTestState *s)
{
    kona_qom_set_uint(s, "x-test-pixel-x", LIST_POINT_X);
    kona_qom_set_uint(s, "x-test-pixel-y", LIST_POINT_Y);
    return kona_qom_get_uint(s, "x-test-pixel");
}

#define DMA_POOL_BASE UINT64_C(0x40000)

/*
 * 0x1ed registers a pool; 0x171 invokes one list from it as two ranges,
 * the first six words at pool offset 0x100 and the other seven at 0x800.
 * Every word reaches the command page's decoder in order, across the
 * break between ranges, so the point is drawn in the loaded colour, and
 * the list completes once, CONSUMED with its 13 words and no cookie.
 */
static void test_list_from_pool_reaches_decoder(void)
{
    kona_reset();
    kona_write_be32_array(qts, DMA_POOL_BASE + 0x100, list_words, 6);
    kona_write_be32_array(qts, DMA_POOL_BASE + 0x800, list_words + 6, 7);
    kona_register_pool(qts, DMA_POOL_BASE, 0x4000);

    kona_command(qts, (const uint32_t[]) {
        LIST_CALLS_POOL, 2, 0x100, 6, 0x800, 7,
    }, 6);

    g_assert_cmphex(kona_point_pixel(qts), ==, LIST_POINT_RGB);
    g_assert_cmpuint(kona_completions(qts), ==, 1);
    kona_assert_last_completion(qts, DONE_CONSUMED, 0, 13);
}

/*
 * The LIST plane path: 0x167 pulls the list from the pool into pipe
 * memory at 0x2000, and 0x170 invokes it from there as two ranges. 0x16f
 * then moves the last four words, the point, to 0x3000, and 0x16d invokes
 * them alone after another colour was selected, redrawing the point in
 * that colour.
 */
static void test_list_from_pipe_memory_reaches_decoder(void)
{
    kona_reset();
    kona_write_be32_array(qts, DMA_POOL_BASE, list_words,
                          ARRAY_SIZE(list_words));
    kona_register_pool(qts, DMA_POOL_BASE, 0x4000);

    kona_command(qts, (const uint32_t[]) {
        LIST_PULL, 0x2000, 0, sizeof(list_words), 0,
    }, 5);
    g_assert_cmphex(kona_list_word(qts, 0x2000), ==, list_words[0]);
    g_assert_cmphex(kona_list_word(qts, 0x2030), ==, list_words[12]);
    kona_assert_last_completion(qts, DONE_WRITTEN, 0, sizeof(list_words));

    kona_command(qts, (const uint32_t[]) {
        LIST_CALLS, 2, 0x2000, 9, 0x2024, 4,
    }, 6);
    g_assert_cmphex(kona_point_pixel(qts), ==, LIST_POINT_RGB);
    kona_assert_last_completion(qts, DONE_CONSUMED, 0, 13);

    kona_command(qts, (const uint32_t[]) { 1, 0x3, 6, 0, 0, 0x3fc }, 6);
    kona_command(qts, (const uint32_t[]) { 1, 0x2, 6 }, 3);
    kona_command(qts, (const uint32_t[]) {
        LIST_MOVE, 0x3000, 0x2024, 16,
    }, 4);
    g_assert_cmphex(kona_list_word(qts, 0x3000), ==, list_words[9]);
    g_assert_cmphex(kona_point_pixel(qts), ==, LIST_POINT_RGB);
    kona_command(qts, (const uint32_t[]) { LIST_CALL, 0x3000, 4 }, 3);
    g_assert_cmphex(kona_point_pixel(qts), ==, 0x0000ff);
    kona_assert_last_completion(qts, DONE_CONSUMED, 0, 4);
}

/*
 * 0x170 runs up to eight ranges of a list in pipe memory as one list:
 * the 13-word list pulled to 0x2000 and invoked as eight ranges draws its
 * point in the loaded colour, CONSUMED with all 13 words, so the fetch
 * takes eight segments. Nine is past the packet's limit and opens
 * nothing: the sync probe after it frames as a probe and is answered.
 */
static void test_list_calls_eight_ranges(void)
{
    static const uint32_t start[8] = { 0, 2, 4, 6, 8, 9, 11, 12 };
    static const uint32_t count[8] = { 2, 2, 2, 2, 1, 2, 1, 1 };
    uint32_t words[1 + 16] = { 8 };

    kona_reset();
    kona_write_be32_array(qts, DMA_POOL_BASE, list_words,
                          ARRAY_SIZE(list_words));
    kona_register_pool(qts, DMA_POOL_BASE, 0x4000);
    kona_command(qts, (const uint32_t[]) {
        LIST_PULL, 0x2000, 0, sizeof(list_words), 0,
    }, 5);
    for (unsigned i = 0; i < 8; i++) {
        words[1 + 2 * i] = 0x2000 + 4 * start[i];
        words[2 + 2 * i] = count[i];
    }
    qtest_writel(qts, REG_COMMAND, LIST_CALLS);
    kona_command(qts, words, ARRAY_SIZE(words));
    g_assert_cmphex(kona_point_pixel(qts), ==, LIST_POINT_RGB);
    kona_assert_last_completion(qts, DONE_CONSUMED, 0, 13);

    kona_command(qts, (const uint32_t[]) { LIST_CALLS, 9, 1, 9, 0 }, 5);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(qts, REG_MAILBOX), ==, 0x1234abcd);
    kona_assert_last_completion(qts, DONE_CONSUMED, 0, 13);
}

#define DMA_BIG_POOL_BASE UINT64_C(0x1000000)

/*
 * A resident list may be as long as the display-list class, 15 MiB of
 * words: one of 2^20 + 1 words, past the shared fetch's default cap, runs
 * whole. One word longer than the class is refused, FAILED with BOUNDS,
 * before anything is read.
 */
static void test_list_longer_than_class_refused(void)
{
    kona_reset();
    kona_register_pool(qts, DMA_BIG_POOL_BASE, 16 * 1024 * 1024);

    kona_command(qts, (const uint32_t[]) {
        LIST_CALL_POOL, 0, (1u << 20) + 1,
    }, 3);
    kona_assert_last_completion(qts, DONE_CONSUMED, 0, (1u << 20) + 1);

    kona_command(qts, (const uint32_t[]) {
        LIST_CALL_POOL, 0, LIST_MAX_WORDS + 1,
    }, 3);
    kona_assert_last_completion(qts, DONE_FAILED, 0, FAIL_BOUNDS);
}

/*
 * A list that invokes a list: the inner invocation finds the fetch
 * running and fails with BUSY, and the outer list completes after it, so
 * its CONSUMED record is the last. Without a pool, a pool invocation is
 * refused with BOUNDS.
 */
static void test_list_nested_and_poolless_refused(void)
{
    static const uint32_t outer[] = { LIST_CALL_POOL, 0x100, 6, 1, 0x2, 5 };

    kona_reset();
    kona_command(qts, (const uint32_t[]) { LIST_CALL_POOL, 0, 4 }, 3);
    kona_assert_last_completion(qts, DONE_FAILED, 0, FAIL_BOUNDS);

    kona_write_be32_array(qts, DMA_POOL_BASE, outer, ARRAY_SIZE(outer));
    kona_register_pool(qts, DMA_POOL_BASE, 0x4000);
    kona_command(qts, (const uint32_t[]) {
        LIST_CALL_POOL, 0, ARRAY_SIZE(outer),
    }, 3);
    g_assert_cmpuint(kona_completions(qts), ==, 3);
    kona_assert_last_completion(qts, DONE_CONSUMED, 0, ARRAY_SIZE(outer));
}

#define DMA_POST_SRC UINT64_C(0x16004)

/*
 * A packet whose word 6 is 0x100 completes as kona_arm_intr expects: the
 * mailbox holds 0x100 under cause bit 0x10, and the read-clear-acknowledge
 * sequence clears both. A packet whose word 6 is 0 posts nothing, and
 * neither does a display list's completion. A refused packet whose
 * sender waits sets the fatal bit 0x100 and posts no mailbox word.
 */
static void test_dma_completion_posts_mailbox(void)
{
    uint64_t addr = dma_node_addr(DMA_POST_SRC);

    kona_reset();
    kona_write_be32_array(qts, addr, dma_pixels, 4);

    kona_dma_packet(qts, addr, 16, 16, 0, 0);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);

    kona_dma_packet(qts, addr, 16, 16, 0, KONA_DMA_DONE_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(qts, REG_MAILBOX), ==, KONA_DMA_DONE_WORD);
    qtest_writel(qts, REG_MAILBOX, 0);
    qtest_writel(qts, REG_INT_CAUSE, CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_MAILBOX), ==, 0);

    kona_write_be32_array(qts, DMA_POOL_BASE, list_words,
                          ARRAY_SIZE(list_words));
    kona_register_pool(qts, DMA_POOL_BASE, 0x4000);
    kona_command(qts, (const uint32_t[]) {
        LIST_CALL_POOL, 0, ARRAY_SIZE(list_words),
    }, 3);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);

    kona_dma_packet(qts, addr, 16, 12, 0, KONA_DMA_DONE_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, CAUSE_FATAL);
    g_assert_cmphex(qtest_readl(qts, REG_MAILBOX), ==, 0);
}

#define DMA_MIGRATE_SRC UINT64_C(0x12004)

/*
 * A list-write record and a packet half-pushed on the source complete on
 * the destination once the packet's last words arrive there: the port's
 * framing state and the pending record both migrate, so the bytes land
 * in the list plane, not the framebuffer.
 */
static void test_dma_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint64_t addr = dma_node_addr(DMA_MIGRATE_SRC);

    tmpfs = g_dir_make_tmp("sn0-kona-dma-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    kona_write_be32_array(from, addr, dma_pixels, 4);
    kona_dma_list_write(from, 0x100, 16);
    kona_dma_push_word(from, KONA_DMA_PACKET);
    kona_dma_push_word(from, (uint32_t)(addr >> 12));
    kona_dma_push_word(from, ((uint32_t)addr & 0xfffu) | KONA_DMA_ADDR_TAG);
    kona_dma_push_word(from, 16);

    sgi_migrate_stopped(from, uri);

    kona_dma_push_word(to, 16);
    kona_dma_push_word(to, 0);
    kona_dma_push_word(to, KONA_DMA_DONE_WORD);

    g_assert_cmphex(kona_list_word(to, 0x100), ==, dma_pixels[0]);
    g_assert_cmphex(kona_list_word(to, 0x10c), ==, dma_pixels[3]);
    g_assert_cmphex(kona_fb_word(to, 0, 0), ==, 0);
    kona_assert_last_completion(to, DONE_WRITTEN, COOKIE_MAILBOX_DONE, 16);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * A registered pool and a 0x170 part-way through its pairs survive a
 * migration: the destination takes the second pair and runs both ranges
 * from pipe memory, drawing the point, and a 0x16e there reads the pool
 * the source registered.
 */
static void test_list_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-dma-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    kona_write_be32_array(from, DMA_POOL_BASE, list_words,
                          ARRAY_SIZE(list_words));
    kona_register_pool(from, DMA_POOL_BASE, 0x4000);
    kona_command(from, (const uint32_t[]) {
        LIST_PULL, 0x2000, 0, sizeof(list_words), 0,
    }, 5);
    kona_command(from, (const uint32_t[]) { LIST_CALLS, 2, 0x2000, 9 }, 4);
    g_assert_cmphex(kona_point_pixel(from), ==, 0);

    sgi_migrate_stopped(from, uri);

    kona_command(to, (const uint32_t[]) { 0x2024, 4 }, 2);
    g_assert_cmphex(kona_point_pixel(to), ==, LIST_POINT_RGB);
    kona_assert_last_completion(to, DONE_CONSUMED, 0, 13);
    kona_command(to, (const uint32_t[]) {
        LIST_CALL_POOL, 0, ARRAY_SIZE(list_words),
    }, 3);
    kona_assert_last_completion(to, DONE_CONSUMED, 0, 13);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona/dma/"
                   "forward-transfer-writes-framebuffer",
                   test_dma_forward_transfer_writes_framebuffer);
    qtest_add_func("sn0/origin2000/kona/dma/lines-and-gap-place-rows",
                   test_dma_lines_and_gap_place_rows);
    qtest_add_func("sn0/origin2000/kona/dma/"
                   "reverse-transfer-reads-framebuffer-to-guest",
                   test_dma_reverse_transfer_reads_framebuffer_to_guest);
    qtest_add_func("sn0/origin2000/kona/dma/"
                   "completion-cookie-follows-word-6",
                   test_dma_completion_cookie_follows_word_6);
    qtest_add_func("sn0/origin2000/kona/dma/tag-mismatch-refused",
                   test_dma_tag_mismatch_refused);
    qtest_add_func("sn0/origin2000/kona/dma/line-wider-than-plane-refused",
                   test_dma_line_wider_than_plane_refused);
    qtest_add_func("sn0/origin2000/kona/dma/shape-constraints-refused",
                   test_dma_shape_constraints_refused);
    qtest_add_func("sn0/origin2000/kona/dma/"
                   "destination-offset-out-of-range-refused",
                   test_dma_destination_offset_out_of_range_refused);
    qtest_add_func("sn0/origin2000/kona/dma/list-write-lands-in-list-plane",
                   test_dma_list_write_lands_in_list_plane);
    qtest_add_func("sn0/origin2000/kona/dma/"
                   "list-write-outside-plane-refused",
                   test_dma_list_write_outside_plane_refused);
    qtest_add_func("sn0/origin2000/kona/dma/stray-words-resynchronize",
                   test_dma_stray_words_resynchronize);
    qtest_add_func("sn0/origin2000/kona/dma/"
                   "partial-packet-holds-state-across-pushes",
                   test_dma_partial_packet_holds_state_across_pushes);
    qtest_add_func("sn0/origin2000/kona/dma/command-page-reaches-engine",
                   test_dma_command_page_reaches_engine);
    qtest_add_func("sn0/origin2000/kona/dma/list-from-pool-reaches-decoder",
                   test_list_from_pool_reaches_decoder);
    qtest_add_func("sn0/origin2000/kona/dma/"
                   "list-from-pipe-memory-reaches-decoder",
                   test_list_from_pipe_memory_reaches_decoder);
    qtest_add_func("sn0/origin2000/kona/dma/list-longer-than-class-refused",
                   test_list_longer_than_class_refused);
    qtest_add_func("sn0/origin2000/kona/dma/"
                   "list-nested-and-poolless-refused",
                   test_list_nested_and_poolless_refused);
    qtest_add_func("sn0/origin2000/kona/dma/completion-posts-mailbox",
                   test_dma_completion_posts_mailbox);
    qtest_add_func("sn0/origin2000/kona/dma/list-calls-eight-ranges",
                   test_list_calls_eight_ranges);
    qtest_add_func("sn0/origin2000/kona/dma/migration", test_dma_migration);
    qtest_add_func("sn0/origin2000/kona/dma/list-migration",
                   test_list_migration);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
