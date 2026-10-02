/*
 * MGRAS register-contract and protocol qtests: pixel transfer, DMA, page-
 * list, context eviction and data-port cases.
 *
 * Unsupported behavior remains an approximation unless the public MGRAS
 * contracts specify it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include <math.h>
#include <sys/wait.h>
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"
#include "sn0-user-image.h"
#include "sgi-mgras-test.h"


typedef struct SIDmaMapSlot {
    uint64_t base;
    uint32_t count;
    uint32_t width;
    uint32_t control;
} SIDmaMapSlot;

/* Selects a DMA source-mapping slot and reads back its stored fields. */
static SIDmaMapSlot mgras_test_dma_map_slot(uint32_t slot)
{
    SIDmaMapSlot result;

    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-dma-map-slot', 'value': %u}}", MGRAS_QOM_PATH,
        slot);
    result.base = mgras_qom_get_uint(qts, "x-test-dma-map-base");
    result.count = mgras_qom_get_uint(qts, "x-test-dma-map-count");
    result.width = mgras_qom_get_uint(qts, "x-test-dma-map-width");
    result.control = mgras_qom_get_uint(qts, "x-test-dma-map-control");
    return result;
}

/*
 * The kernel-privileged mapping port (0x130400) shares 0x120400's
 * class/reg/len token framing. A class-0x48 header names a starting
 * register and payload_bytes/4 consecutive words land there as they
 * arrive; see sgi_mgras_dma_map_write() in hw/misc/sgi-mgras-raster.c.
 */
static void test_dma_map_slot_registers(void)
{
    SIDmaMapSlot slot;

    mgras_reset();

    /*
     * Slot 3's base pair: a header (class 0x48, reg 0x26, 8-byte
     * payload) naming the two consecutive registers 0x26/0x27, then its
     * two payload words as one 8-byte write, high word first.
     */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482608);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00000005 << 32) | 0x77abc000);
    /*
     * Count (reg 0x33), width (reg 0x2e) and control (reg 0x4e): header
     * and single payload word combined into one 8-byte write, header
     * first -- the same framing test_board_bringup_scratch_registers
     * already exercises for this port.
     */
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00483304 << 32) | 0x0000000a);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00482e04 << 32) | 0x00000004);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484e04 << 32) | 0x00000001);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    slot = mgras_test_dma_map_slot(3);
    g_assert_cmphex(slot.base, ==, 0x577abc000ULL);
    g_assert_cmphex(slot.count, ==, 0xa);
    g_assert_cmphex(slot.width, ==, 4);
    g_assert_cmphex(slot.control, ==, 1);

    /* An unrelated slot stays at its post-reset value. */
    slot = mgras_test_dma_map_slot(0);
    g_assert_cmphex(slot.base, ==, 0);
    g_assert_cmphex(slot.count, ==, 0);
}

static void test_dma_map_close_marker_and_discard(void)
{
    SIDmaMapSlot slot;

    mgras_reset();

    /*
     * The kernel-sequence close marker (class 0x0e reg 0x01, no
     * payload) is accepted and ignored on this port.
     */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x000e0100);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    /*
     * An unrecognized class frames and discards its declared payload
     * instead of letting the next word be mis-parsed as a header: the
     * word after this burst is still read as a fresh header.
     */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00010004);
    qtest_writel(qts, MGRAS_REG(0x130400), 0xdeadbeef);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00483004 << 32) | 0x00000007);
    slot = mgras_test_dma_map_slot(0);
    g_assert_cmphex(slot.count, ==, 7);
}

/*
 * Host-memory DMA image transfer: plant a one-entry page list and eight
 * source bytes in node 0's own memory, the node MGRAS itself sits behind
 * (sn-catalogue.json: xio-slot-3, MGRAS's own attachment, is
 * controlling-node 0 -- the same node qtest's first CPU boots on, so no
 * inter-Hub claim needs programming before the plant writes or the
 * engine's own read land). Map that node through DMA-map slot 0 with the
 * Hub's own canonical widget number (9, the same HUB_WIDGET later tests
 * in this file use to reach a Hub's register space by widget-shifted
 * fabric address), since sgi_mgras_xio_compose()'s widget-shifted address
 * targets a Hub, not an XIO card, for a host-memory read. Then kick a
 * class-0x0008 write-direction transfer. This exercises
 * mgras_raster_dma_draw_image()'s host-memory read path end to end: the
 * page-list lookup, sgi_mgras_xio_compose()'s widget-shifted fabric
 * address, and the 32-bit pixel paint, whose red is the pixel's low byte
 * (see mgras_raster_dma_draw_image()). It also confirms the
 * write kick sets the FENCE completion bit the IRIX X server polls
 * after every DMA kick.
 */
#define DMA_XFER_NODE       0u
#define DMA_XFER_WIDGET     9u
#define DMA_XFER_LIST_PAGE  0x2000u
#define DMA_XFER_LIST_BASE  (DMA_XFER_LIST_PAGE | 0x18u)
#define DMA_XFER_SRC_VA     0x7000u

static uint64_t dma_xfer_node_addr(uint32_t offset)
{
    return ((uint64_t)DMA_XFER_NODE << 32) | offset;
}

static void test_dma_image_transfer_paints_from_host_memory(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = { 0, 0, 0, 0, 0, 0, 0, 0x07 };
    static const uint8_t src_pixels[8] = {
        0x00, 0x11, 0x22, 0x33, 0x00, 0x44, 0x55, 0x66,
    };

    mgras_reset();
    mgras_enter_context(qts);

    /*
     * Page list: three
     * zero 8-byte header words, then page-frame-number entries starting
     * at byte 0x18. One entry here, page-frame 7 (0x7000 >> 12). Source
     * pixels live at page 7, offset 0 -- the same bytes the va = 0..7
     * formula resolves to for pitch=8, addr=0, rows=1, bpr=8 below.
     */
    qtest_memwrite(qts, dma_xfer_node_addr(DMA_XFER_LIST_PAGE),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_xfer_node_addr(DMA_XFER_LIST_BASE), list_entry, 8);
    qtest_memwrite(qts, dma_xfer_node_addr(DMA_XFER_SRC_VA), src_pixels, 8);

    /*
     * Destination paint state: copy op, full colour mask, rectangle
     * (50,50)-(51,50).
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (50u << 16) | 50u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (51u << 16) | 50u));

    /* DMA-map slot 0: node 0's list entry-0 address, one page, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                dma_xfer_node_addr(DMA_XFER_LIST_BASE));
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_XFER_WIDGET << 16));

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);

    /*
     * DESC_20000 bank: pitch, addr low/high, rows, bytes/row, then a
     * write-direction kick (bit 3 clear).
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080b04 << 32) | 0);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);

    g_assert_cmphex(mgras_test_pixel(qts, 50, 50), ==, 0x00332211);
    g_assert_cmphex(mgras_test_pixel(qts, 51, 50), ==, 0x00665544);
}

#undef DMA_XFER_NODE
#undef DMA_XFER_WIDGET
#undef DMA_XFER_LIST_PAGE
#undef DMA_XFER_LIST_BASE
#undef DMA_XFER_SRC_VA

/*
 * A replayed session has no guest memory behind it, so sgi_mgras_dma_chunk
 * is the only way a traced capture can carry what a host-to-screen DMA draw
 * actually read. Reuse paints-from-host-memory's exact register sequence
 * (one page-list entry, an 8-byte two-pixel row) under
 * -trace enable=sgi_mgras_dma_chunk and confirm the trace carries both
 * reads the model performs: the one page-list entry (8 bytes at the
 * list's plain node address) and the row's eight source bytes, which lie
 * inside one guest page and so arrive as a single chunk. The traced
 * address is the same plain node-offset address qtest_memwrite() below
 * names, not the fully composed fabric address sgi_mgras_xio_compose()
 * derives from it (widget 9 shifted into the high bits) -- because a
 * replay's own memwrite reaches qtest's guest RAM only at the plain
 * address; the fabric address exists only to route the DMA through
 * s->fabric_as and is not otherwise backed by memory qtest_memwrite() can
 * reach. This needs its own machine rather than the shared `qts`, since
 * trace enablement is a command-line option a running QEMU cannot pick up
 * after the fact.
 */
#define DMA_CHUNK_NODE       0u
#define DMA_CHUNK_WIDGET     9u
#define DMA_CHUNK_LIST_PAGE  0x2000u
#define DMA_CHUNK_LIST_BASE  (DMA_CHUNK_LIST_PAGE | 0x18u)
#define DMA_CHUNK_SRC_VA     0x7000u

static uint64_t dma_chunk_node_addr(uint32_t offset)
{
    return ((uint64_t)DMA_CHUNK_NODE << 32) | offset;
}

static void test_dma_image_transfer_traces_source_chunks(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = { 0, 0, 0, 0, 0, 0, 0, 0x07 };
    static const uint8_t src_pixels[8] = {
        0x00, 0x11, 0x22, 0x33, 0x00, 0x44, 0x55, 0x66,
    };
    g_autofree char *trace_path = NULL;
    g_autofree char *args = NULL;
    g_autofree char *trace_contents = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("dma-chunk-trace-XXXXXX.trace", &trace_path, &error);
    g_assert_no_error(error);
    close(fd);

    args = g_strdup_printf(MACHINE_ARGS " -trace enable=sgi_mgras_dma_chunk"
                          " -D %s", trace_path);
    qts = qtest_init(args);
    mgras_enter_context(qts);

    qtest_memwrite(qts, dma_chunk_node_addr(DMA_CHUNK_LIST_PAGE),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_chunk_node_addr(DMA_CHUNK_LIST_BASE), list_entry,
                  8);
    qtest_memwrite(qts, dma_chunk_node_addr(DMA_CHUNK_SRC_VA), src_pixels, 8);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (50u << 16) | 50u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (51u << 16) | 50u));

    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                dma_chunk_node_addr(DMA_CHUNK_LIST_BASE));
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_CHUNK_WIDGET << 16));

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080b04 << 32) | 0);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(mgras_test_pixel(qts, 50, 50), ==, 0x00332211);
    g_assert_cmphex(mgras_test_pixel(qts, 51, 50), ==, 0x00665544);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace_path, &trace_contents, NULL,
                                      &error));
    g_assert_no_error(error);
    /*
     * addr is the plain node-offset address qtest_memwrite() above named,
     * not the fully composed fabric address sgi_mgras_xio_compose() derives
     * from it (widget 9 shifted into the high bits) -- a replay's own
     * memwrite must target this plain address, since that is the only one
     * qtest's guest RAM actually backs.
     */
    g_assert_nonnull(strstr(trace_contents,
        "addr 0x2018 len 8 hex 0000000000000007"));
    g_assert_nonnull(strstr(trace_contents,
        "addr 0x7000 len 8 hex 0011223300445566"));

    unlink(trace_path);
}

#undef DMA_CHUNK_NODE
#undef DMA_CHUNK_WIDGET
#undef DMA_CHUNK_LIST_PAGE
#undef DMA_CHUNK_LIST_BASE
#undef DMA_CHUNK_SRC_VA

/*
 * SRC_OFF_LOW3 and SRC_OFF_ALIGNED are non-overlapping parts of one byte
 * offset (src & 7, src & ~7), not the low/high halves of a 64-bit
 * address: every other DMA image-transfer test in this file leaves
 * SRC_OFF_ALIGNED at its post-reset zero, so none of them can tell
 * addition from `(aligned << 32) | low3` -- both combine to the same
 * value when the high word is zero. Give SRC_OFF_ALIGNED a nonzero,
 * page-relative, 8-aligned value that lands several pages into the
 * mapping (page-frame index 3 of 4) and SRC_OFF_LOW3 a nonzero
 * sub-8 remainder, so only correct addition selects the fourth
 * page-list entry and the right byte within it; a shift-and-OR would
 * push SRC_OFF_ALIGNED into bits 63:32, produce a page index far past
 * `count`, and leave the destination pixel unpainted.
 */
#define DMA_SRCOFF_NODE       0u
#define DMA_SRCOFF_WIDGET     9u
#define DMA_SRCOFF_LIST_PAGE  0xa000u
#define DMA_SRCOFF_LIST_BASE  (DMA_SRCOFF_LIST_PAGE | 0x18u)
#define DMA_SRCOFF_LOW3       5u
#define DMA_SRCOFF_ALIGNED    0x3000u
#define DMA_SRCOFF_TARGET_PFN 0x37u

static uint64_t dma_srcoff_node_addr(uint32_t offset)
{
    return ((uint64_t)DMA_SRCOFF_NODE << 32) | offset;
}

static void test_dma_image_transfer_src_off_aligned_selects_page(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entries[32] = {
        0, 0, 0, 0, 0, 0, 0, 0,          /* index 0: unused */
        0, 0, 0, 0, 0, 0, 0, 0,          /* index 1: unused */
        0, 0, 0, 0, 0, 0, 0, 0,          /* index 2: unused */
        0, 0, 0, 0, 0, 0, 0, DMA_SRCOFF_TARGET_PFN, /* index 3: target */
    };
    static const uint8_t src_pixel[4] = { 0x00, 0x99, 0x88, 0x77 };

    mgras_reset();
    mgras_enter_context(qts);

    /*
     * SRC_OFF_LOW3 + SRC_OFF_ALIGNED = 5 + 0x3000 = 0x3005, so
     * page_ix = 0x3005 >> 12 = 3 and the source byte offset within
     * that page is 0x3005 & 0xfff = 5. Plant the pixel at page-frame
     * DMA_SRCOFF_TARGET_PFN, byte offset 5.
     */
    qtest_memwrite(qts, dma_srcoff_node_addr(DMA_SRCOFF_LIST_PAGE),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_srcoff_node_addr(DMA_SRCOFF_LIST_BASE),
                  list_entries, sizeof(list_entries));
    qtest_memwrite(qts,
                  dma_srcoff_node_addr((DMA_SRCOFF_TARGET_PFN << 12) + 5),
                  src_pixel, sizeof(src_pixel));

    /* Destination paint state: copy op, full colour mask, pixel (30,30). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (30u << 16) | 30u));

    /* DMA-map slot 0: node 0's list entry-0 address, four pages, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                dma_srcoff_node_addr(DMA_SRCOFF_LIST_BASE));
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00483004 << 32) | 4);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_SRCOFF_WIDGET << 16));

    /*
     * DESC_20000 bank: pitch (unused, one row), SRC_OFF_LOW3,
     * SRC_OFF_ALIGNED, rows, bytes/row, then a write-direction kick
     * naming slot 0 (bits 2:1 clear).
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080504 << 32) | DMA_SRCOFF_LOW3);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080604 << 32) | DMA_SRCOFF_ALIGNED);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080b04 << 32) | 0);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 30, 30), ==, 0x00778899);
}

#undef DMA_SRCOFF_NODE
#undef DMA_SRCOFF_WIDGET
#undef DMA_SRCOFF_LIST_PAGE
#undef DMA_SRCOFF_LIST_BASE
#undef DMA_SRCOFF_LOW3
#undef DMA_SRCOFF_ALIGNED
#undef DMA_SRCOFF_TARGET_PFN

/*
 * Two source-mapping slots held open at once, as every capture that
 * reaches a copy: the DDX keeps a
 * long-lived slot 0 mapping for its small-copy bounce buffer alongside a
 * per-transfer slot 3 mapping for larger copies. The kick byte's bits
 * 2:1 name which one a given transfer reads; a scan for "the only
 * mapped slot" (the model's old behaviour) cannot tell them apart.
 */
#define DMA_SLOT_TEST_NODE   0u
#define DMA_SLOT_TEST_WIDGET 9u

static uint64_t dma_slot_test_node_addr(uint32_t offset)
{
    return ((uint64_t)DMA_SLOT_TEST_NODE << 32) | offset;
}

static void test_dma_image_transfer_kick_byte_selects_slot(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list0[8] = { 0, 0, 0, 0, 0, 0, 0, 0x07 };
    static const uint8_t list3[8] = { 0, 0, 0, 0, 0, 0, 0, 0x09 };
    static const uint8_t pixels0[4] = { 0x00, 0x33, 0x22, 0x11 };
    static const uint8_t pixels3[4] = { 0x00, 0x77, 0x66, 0x55 };

    mgras_reset();
    mgras_enter_context(qts);

    /*
     * Each list has three
     * zero header words at the mapped page's start, then the entry at
     * byte 0x18.
     */
    qtest_memwrite(qts, dma_slot_test_node_addr(0x2000),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_slot_test_node_addr(0x2018), list0, 8);
    qtest_memwrite(qts, dma_slot_test_node_addr(0x7000), pixels0, 4);
    qtest_memwrite(qts, dma_slot_test_node_addr(0x3000),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_slot_test_node_addr(0x3018), list3, 8);
    qtest_memwrite(qts, dma_slot_test_node_addr(0x9000), pixels3, 4);

    /* DMA-map slot 0: node 0's list entry 0 at 0x2018, one page, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400), dma_slot_test_node_addr(0x2018));
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_SLOT_TEST_WIDGET << 16));

    /* DMA-map slot 3: node 0's list entry 0 at 0x3018, one page, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482608);
    qtest_writeq(qts, MGRAS_REG(0x130400), dma_slot_test_node_addr(0x3018));
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483304 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484e04 << 32) | (DMA_SLOT_TEST_WIDGET << 16));

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));

    /* Kick byte 0xb1 (write, bits 2:1 == 0): slot 0's pixel at (10,10). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080b04 << 32) | 0xb1);

    g_assert_cmphex(mgras_test_pixel(qts, 10, 10), ==, 0x00112233);

    /* Kick byte 0xb7 (write, bits 2:1 == 3): slot 3's pixel at (11,10). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (11u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (11u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080b04 << 32) | 0xb7);

    g_assert_cmphex(mgras_test_pixel(qts, 11, 10), ==, 0x00556677);

    /*
     * Kick byte 0xb3 (write, bits 2:1 == 1) names slot 1, never mapped:
     * no transfer, no crash, and the write handshake still completes so
     * the guest's 0x020010 poll returns.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (12u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (12u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080b04 << 32) | 0xb3);

    g_assert_cmphex(mgras_test_pixel(qts, 12, 10), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

#undef DMA_SLOT_TEST_NODE
#undef DMA_SLOT_TEST_WIDGET

/*
 * Page-list base regression: the base pair's low
 * word is entry 0's byte address, three zero 8-byte header words then
 * page-frame-number entries from byte 0x18, not a page-aligned address
 * with flag bits. Plant different pixel bytes at the real page-frame
 * number's page and at physical page 0, so masking the base down to its
 * page (the old bug -- it reads the header's first zero word as
 * page-frame number 0) paints page 0's bytes instead of the real page's.
 */
#define DMA_LIST_BASE_NODE      0u
#define DMA_LIST_BASE_WIDGET    9u
#define DMA_LIST_BASE_LIST_PAGE 0x5000u
#define DMA_LIST_BASE_LIST_BASE (DMA_LIST_BASE_LIST_PAGE | 0x18u)
#define DMA_LIST_BASE_REAL_PFN  0x0bu
#define DMA_LIST_BASE_REAL_PAGE (DMA_LIST_BASE_REAL_PFN << 12)

static uint64_t dma_list_base_node_addr(uint32_t offset)
{
    return ((uint64_t)DMA_LIST_BASE_NODE << 32) | offset;
}

static void test_dma_image_transfer_reads_list_entry_at_0x18(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = {
        0, 0, 0, 0, 0, 0, 0, DMA_LIST_BASE_REAL_PFN,
    };
    static const uint8_t real_page_pixels[4] = { 0x00, 0xcc, 0xbb, 0xaa };
    static const uint8_t page0_pixels[4] = { 0x00, 0x33, 0x22, 0x11 };

    mgras_reset();
    mgras_enter_context(qts);

    qtest_memwrite(qts, dma_list_base_node_addr(DMA_LIST_BASE_LIST_PAGE),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_list_base_node_addr(DMA_LIST_BASE_LIST_BASE),
                  list_entry, 8);
    qtest_memwrite(qts, dma_list_base_node_addr(DMA_LIST_BASE_REAL_PAGE),
                  real_page_pixels, 4);
    qtest_memwrite(qts, dma_list_base_node_addr(0), page0_pixels, 4);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (20u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (20u << 16) | 20u));

    /* DMA-map slot 0: base names entry 0 at byte 0x18, one page, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                dma_list_base_node_addr(DMA_LIST_BASE_LIST_BASE));
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_LIST_BASE_WIDGET << 16));

    /*
     * DESC_20000 bank: pitch, addr low/high, rows, bytes/row, then a
     * write-direction kick naming slot 0 (bits 2:1 clear).
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080b04 << 32) | 0xb1);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 20, 20), ==, 0x00aabbcc);
}

#undef DMA_LIST_BASE_NODE
#undef DMA_LIST_BASE_WIDGET
#undef DMA_LIST_BASE_LIST_PAGE
#undef DMA_LIST_BASE_LIST_BASE
#undef DMA_LIST_BASE_REAL_PFN
#undef DMA_LIST_BASE_REAL_PAGE

/*
 * A transfer whose rows run past the slot's page list moves nothing, in
 * either direction. One mapped page and a two-pixel, 8-byte row starting
 * at buffer offset 0xffc: the first pixel's bytes are the page's last
 * four, the second pixel's lie past the list. The whole transfer fails
 * its bounds check before any guest memory is resolved, so the draw
 * paints neither pixel and the read writes none of the page. The draw
 * still raises the fence, because the kick has finished with the
 * descriptor either way.
 */
#define DMA_OVERRUN_WIDGET    9u
#define DMA_OVERRUN_LIST_PAGE 0xc000u
#define DMA_OVERRUN_LIST_BASE (DMA_OVERRUN_LIST_PAGE | 0x18u)
#define DMA_OVERRUN_PFN       0x40u
#define DMA_OVERRUN_PAGE      (DMA_OVERRUN_PFN << 12)

static void dma_overrun_descriptor(uint32_t kick)
{
    /* Pitch 8, offset 4 + 0xff8, one row of 8 bytes. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080604 << 32) | 0xff8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080b04 << 32) | kick);
}

static void test_dma_image_transfer_past_page_list_moves_nothing(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = {
        0, 0, 0, 0, 0, 0, 0, DMA_OVERRUN_PFN,
    };
    static const uint8_t last_pixel[4] = { 0x00, 0x33, 0x22, 0x11 };
    uint8_t page_end[4];

    mgras_reset();
    mgras_enter_context(qts);

    qtest_memwrite(qts, DMA_OVERRUN_LIST_PAGE, list_header,
                   sizeof(list_header));
    qtest_memwrite(qts, DMA_OVERRUN_LIST_BASE, list_entry, 8);
    qtest_memwrite(qts, DMA_OVERRUN_PAGE + 0xffc, last_pixel, 4);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (61u << 16) | 60u));

    /* DMA-map slot 0: entry 0 at 0xc018, one page, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400), DMA_OVERRUN_LIST_BASE);
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484b04 << 32) | (DMA_OVERRUN_WIDGET << 16));

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    dma_overrun_descriptor(0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(mgras_test_pixel(qts, 60, 60), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 61, 60), ==, 0);

    /*
     * Paint both pixels, then read them back over the same descriptor:
     * the hardware-y box (60,963)-(61,963) is screen row 60. The page's
     * last four bytes keep what the guest put there.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 7));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (61u << 16) | 60u));
    g_assert_cmphex(mgras_test_pixel(qts, 60, 60), ==, 7);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (60u << 16) | 963u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (61u << 16) | 963u));
    dma_overrun_descriptor(0xb9);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    qtest_memread(qts, DMA_OVERRUN_PAGE + 0xffc, page_end, sizeof(page_end));
    g_assert_cmpmem(page_end, sizeof(page_end), last_pixel,
                    sizeof(last_pixel));
}

#undef DMA_OVERRUN_WIDGET
#undef DMA_OVERRUN_LIST_PAGE
#undef DMA_OVERRUN_LIST_BASE
#undef DMA_OVERRUN_PFN
#undef DMA_OVERRUN_PAGE

/*
 * A DMA draw paints through the same rules as any other paint: the
 * PP1FILLMODE logic op, the COLORMASKLSBSA write mask, and a format tag
 * that follows the transfer's pixel width. Each case seeds its pixels with
 * an RGB fill of 0x0f0f0f, so the op and mask have something to combine
 * with and the tag has something to change from, then draws one row from
 * a one-page mapping at widget 9.
 */
#define DMA_RULES_WIDGET    9u
#define DMA_RULES_LIST_PAGE 0xd000u
#define DMA_RULES_LIST_BASE (DMA_RULES_LIST_PAGE | 0x18u)
#define DMA_RULES_PFN       0x50u
#define DMA_RULES_PAGE      (DMA_RULES_PFN << 12)
#define DMA_RULES_SEED      0x0f

static void dma_rules_seed(uint32_t x0, uint32_t x1, uint32_t y)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) |
                               MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, DMA_RULES_SEED << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_G, DMA_RULES_SEED << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_B, DMA_RULES_SEED << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (x0 << 16) | y));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (x1 << 16) | y));
    /* Leave rect fill off, or the draw's own block execute fills again. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLMODE, 0));
    for (uint32_t x = x0; x <= x1; x++) {
        g_assert_cmphex(mgras_test_pixel(qts, x, y), ==, 0x000f0f0f);
        g_assert_cmphex(mgras_test_format(qts, x, y), ==,
                        MGRAS_PIXEL_FORMAT_RGB);
    }
}

/* Draw one row of @len guest bytes to @npix pixels from (x0, y). */
static void dma_rules_draw(uint32_t x0, uint32_t y, uint32_t npix,
                           const uint8_t *src, uint32_t len, uint8_t op,
                           uint32_t mask)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = {
        0, 0, 0, 0, 0, 0, 0, DMA_RULES_PFN,
    };

    qtest_memwrite(qts, DMA_RULES_LIST_PAGE, list_header,
                   sizeof(list_header));
    qtest_memwrite(qts, DMA_RULES_LIST_BASE, list_entry, 8);
    qtest_memwrite(qts, DMA_RULES_PAGE, src, len);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, mask));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (uint32_t)op << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (x0 << 16) | y));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END,
                                    ((x0 + npix - 1) << 16) | y));

    /* DMA-map slot 0: entry 0 at 0xd018, one page, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400), DMA_RULES_LIST_BASE);
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484b04 << 32) | (DMA_RULES_WIDGET << 16));

    /* Pitch, offset 0, one row of len bytes, draw kick naming slot 0. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080404 << 32) | len);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080804 << 32) | len);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080b04 << 32) | 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_dma_image_transfer_applies_logic_op(void)
{
    /* Red 0xff, green 0x00, blue 0xff, red in the low byte. */
    static const uint8_t src[4] = { 0x00, 0xff, 0x00, 0xff };

    mgras_reset();
    mgras_enter_context(qts);
    dma_rules_seed(70, 70, 70);
    dma_rules_draw(70, 70, 1, src, sizeof(src), MGRAS_LOGIC_OP_XOR,
                   0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 70, 70), ==, 0x00f00ff0);
}

static void test_dma_image_transfer_applies_colour_mask(void)
{
    static const uint8_t src[4] = { 0x00, 0xc0, 0xb0, 0xa0 };

    mgras_reset();
    mgras_enter_context(qts);
    dma_rules_seed(72, 72, 70);
    dma_rules_draw(72, 70, 1, src, sizeof(src), MGRAS_LOGIC_OP_COPY,
                   0x0000ff00);
    g_assert_cmphex(mgras_test_pixel(qts, 72, 70), ==, 0x000fb00f);
}

static void test_dma_image_transfer_draws_ci8_indices(void)
{
    static const uint8_t src[2] = { 0x05, 0xc3 };

    mgras_reset();
    mgras_enter_context(qts);
    dma_rules_seed(74, 75, 70);
    dma_rules_draw(74, 70, 2, src, sizeof(src), MGRAS_LOGIC_OP_COPY,
                   0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 74, 70), ==, 0x05);
    g_assert_cmphex(mgras_test_pixel(qts, 75, 70), ==, 0xc3);
    g_assert_cmphex(mgras_test_format(qts, 74, 70), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_test_format(qts, 75, 70), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
}

/*
 * A DMA draw names bank 0's plane, and the shared code bounds it by that
 * plane: the plane is the whole 1280x1024 screen, so a CI8 row the
 * screen's full width and an RGB pair ending at the bottom-right pixel
 * both land. They land in bank 0 only: with the XMAP main buffer-select
 * mask showing bank 1 for display ID 0, the RGB pair's pixels show the
 * empty bank.
 */
static void test_dma_image_transfer_lands_in_bank_zero(void)
{
    static const uint8_t rgb[8] = {
        0x00, 0x11, 0x22, 0x33, 0x00, 0x44, 0x55, 0x66,
    };
    uint8_t row[1280];
    uint32_t shown;

    for (uint32_t x = 0; x < sizeof(row); x++) {
        row[x] = x * 7 + 1;
    }

    mgras_reset();
    mgras_enter_context(qts);
    /* Display on, so the display pixel resolves a bank. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    dma_rules_draw(0, 1022, sizeof(row), row, sizeof(row),
                   MGRAS_LOGIC_OP_COPY, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 0, 1022), ==, row[0]);
    g_assert_cmphex(mgras_test_pixel(qts, 640, 1022), ==, row[640]);
    g_assert_cmphex(mgras_test_pixel(qts, 1279, 1022), ==, row[1279]);

    dma_rules_draw(1278, 1023, 2, rgb, sizeof(rgb), MGRAS_LOGIC_OP_COPY,
                   0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 1278, 1023), ==, 0x00332211);
    g_assert_cmphex(mgras_test_pixel(qts, 1279, 1023), ==, 0x00665544);

    shown = mgras_test_display_pixel(qts, 1279, 1023);
    g_assert_cmphex(shown, !=, 0);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u << 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 1279, 1023), ==, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 1279, 1023), ==, shown);
}

#undef DMA_RULES_WIDGET
#undef DMA_RULES_LIST_PAGE
#undef DMA_RULES_LIST_BASE
#undef DMA_RULES_PFN
#undef DMA_RULES_PAGE
#undef DMA_RULES_SEED

/*
 * Screen-to-host read: the read
 * descriptor, hw-y box (0,1023)-(3,1020), pitch 4, over the seed weave
 * painted directly at screen (0,0)-(3,3). Checks
 * the read lands the box top-down at pitch 4 in host memory and leaves
 * 0x020010 bit 16 alone, then replays the paired 0xb1 write -- the same
 * host bytes, reusing the read's page-list mapping -- to confirm the
 * round trip reproduces the weave beside itself, the same doubling
 * the pixel-copy routine's OC variant's first kick pair does in the capture.
 */
#define DMA_READ_NODE      0u
#define DMA_READ_WIDGET    9u
#define DMA_READ_LIST_PAGE 0x6000u
#define DMA_READ_LIST_BASE (DMA_READ_LIST_PAGE | 0x18u)
#define DMA_READ_DATA_PFN  0x30u
#define DMA_READ_DATA_PAGE (DMA_READ_DATA_PFN << 12)

static uint64_t dma_read_node_addr(uint32_t offset)
{
    return ((uint64_t)DMA_READ_NODE << 32) | offset;
}

static void test_dma_image_transfer_reads_screen_to_host(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = {
        0, 0, 0, 0, 0, 0, 0, DMA_READ_DATA_PFN,
    };
    /* Seed pattern: one set pixel per row. */
    static const uint8_t weave[16] = {
        0x00, 0x00, 0x00, 0x07,
        0x00, 0x07, 0x00, 0x00,
        0x00, 0x00, 0x07, 0x00,
        0x07, 0x00, 0x00, 0x00,
    };
    uint8_t got[16];

    mgras_reset();
    mgras_enter_context(qts);

    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_LIST_PAGE),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_LIST_BASE),
                  list_entry, 8);

    /* Paint the weave directly at screen (0,0)-(3,3): base 0, then 7s. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (3u << 16) | 3u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 7));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (3u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (3u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (1u << 16) | 1u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (1u << 16) | 1u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (2u << 16) | 2u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (2u << 16) | 2u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 3u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (0u << 16) | 3u));

    g_assert_cmphex(mgras_test_pixel(qts, 3, 0), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 1, 1), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 2, 2), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 0, 3), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 0, 0), ==, 0);

    /* DMA-map slot 0: node 0's list entry 0 at 0x6018, one page, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                dma_read_node_addr(DMA_READ_LIST_BASE));
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_READ_WIDGET << 16));

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);

    /*
     * Read descriptor: hw-y box (0,1023)-(3,1020), pitch 4, kick 0xb9 (slot 0,
     * read).
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 1023u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (3u << 16) | 1020u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080b04 << 32) | 0xb9);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);

    qtest_memread(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                 got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), weave, sizeof(weave));

    /*
     * Paired write: reuses pitch (reg 4)
     * from the read unwritten, same host bytes, box (4,0)-(7,3) -- the
     * capture's first doubling, weave beside itself.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (4u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (7u << 16) | 3u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080b04 << 32) | 0xb1);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(mgras_test_pixel(qts, 7, 0), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 5, 1), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 6, 2), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 4, 3), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 4, 0), ==, 0);
}

/*
 * The IRIX 6.5.30 greeter's list background: a 16x16 grey dither seeded by
 * a two-pixel-per-execute byte transfer under RGB (PP1FILLMODE 0xc006204,
 * CHAR_H/CHAR_L 0x00c0c0c0/0x00c1c1c1), then doubled across the list by
 * 4-byte-per-pixel DMA copies (read PP1FILLMODE 0x2200, kick 0xb9; draw
 * 0xc006204, kick 0xb1). Each register is one whole RGB pixel. Keeping
 * only its low byte as a CI8 index made the seed look right through a grey
 * colour map, but the copy then read 00 00 00 c0 back and drew it as RGB,
 * so the doubled area showed the grey level in the blue channel alone.
 */
static void test_byte_row_pairs_rgb_survives_dma_copy(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = {
        0, 0, 0, 0, 0, 0, 0, DMA_READ_DATA_PFN,
    };
    /* Big-endian 32-bit pixels, the top byte zero. */
    static const uint8_t row0[8] = {
        0x00, 0xc0, 0xc0, 0xc0, 0x00, 0xc1, 0xc1, 0xc1,
    };
    static const uint8_t coalesced[16] = {
        0x00, 0xc0, 0xc0, 0xc0, 0x00, 0xc1, 0xc1, 0xc1,
        0x00, 0xc1, 0xc1, 0xc1, 0x00, 0xc0, 0xc0, 0xc0,
    };
    static const uint8_t unchanged[16] = {
        0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a,
        0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a,
    };
    uint8_t got[16];

    mgras_reset();
    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    /* A non-grey CI8 entry at 0xc0, so a CI8 misreading cannot pass. */
    mgras_write_cmap_entry(qts, 29, 0xc0, 0x11223300);

    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_LIST_PAGE),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_LIST_BASE),
                  list_entry, 8);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, 0x0c006204));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BYTE_XFER_PACKING,
                           MGRAS_BYTE_XFER_PACKING_PAIRS));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_END, (3u << 16) | 3u));
    for (int row = 0; row < 4; row++) {
        for (int unit = 0; unit < 2; unit++) {
            uint32_t even = row & 1 ? 0x00c1c1c1 : 0x00c0c0c0;
            uint32_t odd = row & 1 ? 0x00c0c0c0 : 0x00c1c1c1;

            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                        MGRAS_COMMAND(REG_CHAR_H, even));
            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                        MGRAS_COMMAND_EXEC(REG_CHAR_L, odd));
        }
    }

    g_assert_cmphex(mgras_test_pixel(qts, 0, 0), ==, 0x00c0c0c0);
    g_assert_cmphex(mgras_test_pixel(qts, 1, 0), ==, 0x00c1c1c1);
    g_assert_cmphex(mgras_test_pixel(qts, 0, 1), ==, 0x00c1c1c1);
    g_assert_cmphex(mgras_test_format(qts, 0, 0), ==, MGRAS_PIXEL_FORMAT_RGB);
    g_assert_cmphex(mgras_test_display_pixel(qts, 0, 0), ==, 0x00c0c0c0);

    /* DMA-map slot 0: node 0's list entry 0 at 0x6018, one page, widget 9. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                dma_read_node_addr(DMA_READ_LIST_BASE));
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_READ_WIDGET << 16));

    /* Read the seed, hw-y box (0,1023)-(3,1020), 4 bytes per pixel. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, 0x00002200));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 1023u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (3u << 16) | 1020u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 16);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 16);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080b04 << 32) | 0xb9);

    qtest_memread(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                 got, sizeof(got));
    g_assert_cmpmem(got, 8, row0, sizeof(row0));


    /* Draw the same bytes back beside the seed, box (4,0)-(7,3). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, 0x0c006204));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (4u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (7u << 16) | 3u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080b04 << 32) | 0xb1);

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            g_assert_cmphex(mgras_test_pixel(qts, 4 + col, row), ==,
                            mgras_test_pixel(qts, col, row));
            g_assert_cmphex(mgras_test_display_pixel(qts, 4 + col, row), ==,
                            mgras_test_display_pixel(qts, col, row));
        }
    }
    g_assert_cmphex(mgras_test_display_pixel(qts, 4, 0), ==, 0x00c0c0c0);

    memset(got, 0xa5, sizeof(got));
    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                   got, sizeof(got));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 1023u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (1u << 16) | 1022u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 16);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 16);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080b04 << 32) | 0xb9);
    qtest_memread(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                  got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), coalesced, sizeof(coalesced));

    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                   unchanged, sizeof(unchanged));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080b04 << 32) | 0xb9);
    qtest_memread(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                  got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), unchanged, sizeof(unchanged));
}

/*
 * A screen-to-host read of an RGB pixel is the inverse of the draw: bytes
 * 1, 2 and 3 of each 4-byte pixel carry blue, green and red, and byte 0
 * reads zero. A red-first read would send a copied colour image back with
 * red and blue exchanged.
 */
static void test_dma_image_transfer_reads_rgb_red_in_low_byte(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = {
        0, 0, 0, 0, 0, 0, 0, DMA_READ_DATA_PFN,
    };
    static const uint8_t want[4] = { 0x00, 0x56, 0x34, 0x12 };
    static const uint8_t poison[4] = { 0xee, 0xee, 0xee, 0xee };
    uint8_t got[4];

    mgras_reset();
    mgras_enter_context(qts);

    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_LIST_PAGE),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_LIST_BASE),
                  list_entry, 8);
    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                  poison, sizeof(poison));

    /* An RGB fill of red 0x12, green 0x34, blue 0x56 at (0,0). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) |
                                               MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_R, 0x12u << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_G, 0x34u << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_B, 0x56u << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (0u << 16) | 0u));
    g_assert_cmphex(mgras_test_pixel(qts, 0, 0), ==, 0x00123456);

    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                dma_read_node_addr(DMA_READ_LIST_BASE));
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_READ_WIDGET << 16));

    /* Read hw-y box (0,1023)-(0,1023): screen pixel (0,0), 4 bytes. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, 0x01000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, 0x00002200));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 1023u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (0u << 16) | 1023u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080b04 << 32) | 0xb9);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    qtest_memread(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                 got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), want, sizeof(want));
}

#undef DMA_READ_NODE
#undef DMA_READ_WIDGET
#undef DMA_READ_LIST_PAGE
#undef DMA_READ_LIST_BASE
#undef DMA_READ_DATA_PFN
#undef DMA_READ_DATA_PAGE


static void test_direct_pio_pixel_pairs(void)
{
    static const uint32_t image[4][4] = {
        { 0x00000000, 0x00ff0000, 0x0000ff00, 0x000000ff },
        { 0x00ffffff, 0x00808080, 0x00ffff00, 0x0000ffff },
        { 0x00ff00ff, 0x00800000, 0x00008000, 0x00000080 },
        { 0x00c0c0c0, 0x00804000, 0x00408000, 0x00004080 },
    };

    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (160u << 16) | 96u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (163u << 16) | 99u));

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x += 2) {
            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                         MGRAS_COMMAND(REG_BKGRD_RG, image[y][x]));
            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                         MGRAS_COMMAND_EXEC(REG_BKGRD_BA, image[y][x + 1]));
        }
    }

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            g_assert_cmphex(mgras_test_pixel(qts, 160 + x, 96 + y), ==,
                            image[y][x]);
        }
    }

    g_assert_cmphex(mgras_test_pixel(qts, 170, 96), ==, 0);
}

/*
 * Display lists. The probe's list case compiles a 16 by 16 grid of
 * triangles, one in each 8-pixel cell, alternately yellow and white, and
 * glCallList sends 0x00080c08, a pool byte offset and a size, after the
 * kernel installs the pool's page list in DMA-map slot 4 (a captured run). The
 * pool words below are laid out the way the board read them at the execute in
 * a captured run (its sgi_mgras_dma_chunk records): each range begins with the
 * header the client's range-header routine writes, 0x00080c08 and the next
 * range's offset and size, or 0x00083904 and 0 on a list's last range, then the
 * port words the list stores, packets whole. The page list is the
 * kernel's (three zero header words, then big-endian page frame
 * numbers), and the pages are neither adjacent nor in address order, as
 * the dump's were not.
 */
#define GL_LIST_EXECUTE_HDR   0x00080c08
#define GL_LIST_NEXT_HDR      0x00080c08
#define GL_LIST_LAST_HDR      0x00083904
#define GL_LIST_CALL_HDR      0x00083708
#define GL_LIST_WIDGET        9u
#define GL_LIST_PAGE_LIST     0x100000u
#define GL_LIST_POOL_PFN(i)   (0x1c0u - 2 * (i))
#define GL_LIST_CELL          8
#define GL_LIST_GRID          16
#define GL_LIST_TRIANGLE_WORDS 13

/* The pool, n pages, as the kernel installs it in slot 4. */
static void gl_list_install(QTestState *qts, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) {
        uint8_t entry[8] = { 0 };

        stl_be_p(entry + 4, GL_LIST_POOL_PFN(i));
        qtest_memwrite(qts, GL_LIST_PAGE_LIST + 0x18 + 8 * i, entry, 8);
        qtest_memset(qts, (uint64_t)GL_LIST_POOL_PFN(i) << 12, 0, 4096);
    }
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482808);
    qtest_writeq(qts, MGRAS_REG(0x130400), GL_LIST_PAGE_LIST | 0x19);
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483404 << 32) | n);
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00482f04 << 32) | 0xa);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484f04 << 32) | (GL_LIST_WIDGET << 16));
}

/* Store words at a pool byte offset, through the page list. */
static void gl_list_put(QTestState *qts, uint32_t offset, const uint32_t *w,
                        unsigned int n)
{
    for (unsigned int i = 0; i < n; i++, offset += 4) {
        uint8_t bytes[4];

        stl_be_p(bytes, w[i]);
        qtest_memwrite(qts, ((uint64_t)GL_LIST_POOL_PFN(offset >> 12) << 12) |
                       (offset & 0xfff), bytes, 4);
    }
}

static float gl_ndc(int window)
{
    return (2.0f * window - 128.0f) / 128.0f;
}

/* Triangle k of the grid, row by row, as the list stores it. */
static void gl_list_triangle(unsigned int k, uint32_t *w)
{
    static const int corner[] = { 0, 0, 7, 2, 3, 8 };
    int row = k / GL_LIST_GRID, column = k % GL_LIST_GRID;
    bool yellow = (row + column) % 2 == 0;

    *w++ = GL_COLOR3F_HDR;
    *w++ = gl_bits(1);
    *w++ = gl_bits(1);
    *w++ = gl_bits(yellow ? 0 : 1);
    for (int i = 0; i < 3; i++) {
        *w++ = GL_VERTEX2F_HDR;
        *w++ = gl_bits(gl_ndc(column * GL_LIST_CELL + corner[2 * i]));
        *w++ = gl_bits(gl_ndc(row * GL_LIST_CELL + corner[2 * i + 1]));
    }
}

/*
 * Pool words from offset: the header words, glBegin when first, triangles
 * first to first + count - 1, glEnd when last. Returns the words written.
 */
typedef void GlListTriangle(unsigned int k, uint32_t *w);

static unsigned int gl_list_page(QTestState *qts, uint32_t offset,
                                 const uint32_t *header, unsigned int hwords,
                                 GlListTriangle *triangle, unsigned int first,
                                 unsigned int count, bool begin, bool end)
{
    g_autofree uint32_t *w = g_new(uint32_t, hwords + 2 +
                                   count * GL_LIST_TRIANGLE_WORDS);
    unsigned int n = 0;

    memcpy(w, header, hwords * sizeof(*w));
    n = hwords;
    if (begin) {
        w[n++] = GL_BEGIN_TRIANGLES;
    }
    for (unsigned int k = first; k < first + count; k++) {
        triangle(k, w + n);
        n += GL_LIST_TRIANGLE_WORDS;
    }
    if (end) {
        w[n++] = GL_END_TRIANGLES;
    }
    gl_list_put(qts, offset, w, n);
    return n;
}

/* The probe's list frame up to glCallList: a blue clear, flat shading. */
static void gl_list_frame(QTestState *qts)
{
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_one(qts, GL_DITHER_HDR, 0);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
}

static void gl_call_list(QTestState *qts, uint32_t offset, uint32_t size)
{
    gl_word(qts, GL_LIST_EXECUTE_HDR);
    gl_word(qts, offset);
    gl_word(qts, size);
}

/*
 * The probe's list as the board reads it: four pages, each beginning
 * with its header words (0x00080c08, the next range's offset and size,
 * or 0x00083904 and 0 on the last), then whole
 * packets: glBegin and triangles 0-77, 78-155, 156-233, then 234-255 and
 * glEnd. The client sent one execute, offset 0 and size 0x3fa, which
 * covers page 0 alone. The window must be all 256 triangles on the
 * clear: the SHA-256 is of the 128x128 RGB bytes gl_min_expected.py
 * writes for the list case (list.ppm), never of this model's output.
 */
#define GL_LIST_SHA256 \
    "1b59646c9996fa22c44a39337023d4611ff83766279777269ad28b69d94f92f7"

static void gl_list_probe_pool(QTestState *qts)
{
    static const uint32_t header[4][3] = {
        { GL_LIST_NEXT_HDR, 0x1000, 0x3f9 },
        { GL_LIST_NEXT_HDR, 0x2000, 0x3f9 },
        { GL_LIST_NEXT_HDR, 0x3000, 0x121 }, { GL_LIST_LAST_HDR, 0 },
    };
    static const unsigned int first[] = { 0, 78, 156, 234, 256 };
    static const uint32_t size[] = { 0x3fa, 0x3f9, 0x3f9, 0x121 };

    gl_list_install(qts, 4);
    for (int i = 0; i < 4; i++) {
        g_assert_cmpuint(gl_list_page(qts, 0x1000 * i, header[i], i < 3 ? 3 : 2,
                                      gl_list_triangle, first[i],
                                      first[i + 1] - first[i], i == 0, i == 3),
                         ==, size[i]);
    }
}

static void test_gl_list_draws_the_probe_list(void)
{
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_list_frame(qts);
    gl_list_probe_pool(qts);
    gl_call_list(qts, 0, 0x3fa);
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_window_sha256(rgb, GL_LIST_SHA256);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The chain decides the order, not the pool: the same list with its
 * ranges at pool offsets 0x3800, 0x0100, 0x1400 and 0x2800, each header
 * naming the next, draws the same window. The first range runs from pool
 * page 3 into page 4, and none starts on a page. Calling the list a
 * second time, as list-twice does, draws the same pixels.
 */
static void test_gl_list_follows_the_chain(void)
{
    static const uint32_t header[4][3] = {
        { GL_LIST_NEXT_HDR, 0x100, 0x3f9 },
        { GL_LIST_NEXT_HDR, 0x1400, 0x3f9 },
        { GL_LIST_NEXT_HDR, 0x2800, 0x121 }, { GL_LIST_LAST_HDR, 0 },
    };
    static const uint32_t offset[] = { 0x3800, 0x100, 0x1400, 0x2800 };
    static const unsigned int first[] = { 0, 78, 156, 234, 256 };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 5);
    for (int i = 0; i < 4; i++) {
        gl_list_page(qts, offset[i], header[i], i < 3 ? 3 : 2,
                     gl_list_triangle, first[i], first[i + 1] - first[i],
                     i == 0, i == 3);
    }
    gl_call_list(qts, 0x3800, 0x3fa);
    gl_call_list(qts, 0x3800, 0x3fa);
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_window_sha256(rgb, GL_LIST_SHA256);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The probe's list-big case as a captured run sent it
 * and its pool dump holds it. A small list with cell (0, 0)'s triangle
 * alone sits at offset 0: two header words, its 15 words and 15
 * 0x000e0100 words, size 32. The big list, 1,024 triangles in 4-pixel
 * cells in three colours, starts at offset 0x1000 and runs through 14
 * pages: 78 triangles on each of the first 13, whose headers name the
 * next page and size 0x3f9 or, before the last, 0x85, and the last 10
 * triangles and glEnd on page 14. The client called the small list
 * (0, 0x20), then the big one (0x1000, 0x3fa). The SHA-256 is of the
 * window gl_min_expected.py writes for list-big, which Mesa llvmpipe
 * matches, never of this model's output.
 */
#define GL_LIST_BIG_SHA256 \
    "604350581efa049c789a5c920a7a499fb52b1c39ddbe6bfcb4f628a281a7980a"

static void gl_list_big_triangle(unsigned int k, uint32_t *w)
{
    static const int corner[] = { 0, 0, 4, 0, 3, 4 };
    static const float rgb[3][3] = { { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 } };
    int row = k / 32, column = k % 32;
    const float *c = rgb[(row + column) % 3];

    *w++ = GL_COLOR3F_HDR;
    for (int i = 0; i < 3; i++) {
        *w++ = gl_bits(c[i]);
    }
    for (int i = 0; i < 3; i++) {
        *w++ = GL_VERTEX2F_HDR;
        *w++ = gl_bits(gl_ndc(column * 4 + corner[2 * i]));
        *w++ = gl_bits(gl_ndc(row * 4 + corner[2 * i + 1]));
    }
}

static void test_gl_list_big_matches_the_reference(void)
{
    uint32_t small[32] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES };
    g_autofree uint32_t *rgb = NULL;

    gl_list_big_triangle(0, small + 3);
    small[16] = GL_END_TRIANGLES;
    for (int i = 17; i < 32; i++) {
        small[i] = 0x000e0100;
    }

    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 16);
    gl_list_put(qts, 0, small, ARRAY_SIZE(small));
    for (unsigned int page = 1; page <= 14; page++) {
        uint32_t header[3] = { GL_LIST_NEXT_HDR, (page + 1) << 12,
                               page == 13 ? 0x85 : 0x3f9 };
        unsigned int first = (page - 1) * 78;
        unsigned int n;

        if (page == 14) {
            header[0] = GL_LIST_LAST_HDR;
            header[1] = 0;
        }
        n = gl_list_page(qts, page << 12, header, page == 14 ? 2 : 3,
                         gl_list_big_triangle, first, MIN(78, 1024 - first),
                         page == 1, page == 14);
        g_assert_cmpuint(n, ==, page == 1 ? 0x3fa : page == 14 ? 0x85 :
                         0x3f9);
    }
    gl_call_list(qts, 0, ARRAY_SIZE(small));
    gl_call_list(qts, 0x1000, 0x3fa);
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_window_sha256(rgb, GL_LIST_BIG_SHA256);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A page-list entry is the page's whole frame number, node included. On
 * more than one node, as a captured run had four, a
 * pool page on node 1 is frame (1 << 20) | f: SN0's node number is
 * physical address bits 39:32. Two nodes are enough; the peer Hub is
 * given node number 1, as the PROM would, before anything reaches it.
 * The page list itself is on node 0. The list on node 1 draws yellow;
 * node 0's frame f holds a list that draws red, which is what taking the
 * node from the list's own address would read.
 */
#define GL_LIST_NODE_ARGS \
    "-M origin2000,topology=origin2000-deskside,nodes=2 -smp 4 -m 128M " \
    "-device sgi-mgras,slot=io3"
#define GL_LIST_NODE_FRAME 0x1c0u

static void gl_list_window_triangle(QTestState *t, uint64_t addr,
                                    float red, float green)
{
    uint32_t w[] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR,
                     gl_bits(red), gl_bits(green), gl_bits(0),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(3), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(3),
                     GL_END_TRIANGLES };
    uint8_t bytes[sizeof(w)];

    for (int i = 0; i < ARRAY_SIZE(w); i++) {
        stl_be_p(bytes + 4 * i, w[i]);
    }
    qtest_memwrite(t, addr, bytes, sizeof(bytes));
}

static void gl_list_page_on_node(unsigned int nasid)
{
    uint64_t node = ((uint64_t)nasid << 32) |
                    ((uint64_t)GL_LIST_NODE_FRAME << 12);
    uint8_t entry[8];
    QTestState *t = qtest_init(GL_LIST_NODE_ARGS);

    sn0_program_nasid(t, nasid);
    stq_be_p(entry, ((uint64_t)nasid << 20) | GL_LIST_NODE_FRAME);
    gl_list_frame(t);
    qtest_memwrite(t, GL_LIST_PAGE_LIST + 0x18, entry, sizeof(entry));
    gl_list_window_triangle(t, node, 1, 1);
    gl_list_window_triangle(t, (uint64_t)GL_LIST_NODE_FRAME << 12, 1, 0);
    qtest_writel(t, MGRAS_REG(0x130400), 0x00482808);
    qtest_writeq(t, MGRAS_REG(0x130400), GL_LIST_PAGE_LIST | 0x19);
    qtest_writeq(t, MGRAS_REG(0x130400), ((uint64_t)0x00483404 << 32) | 1);
    qtest_writeq(t, MGRAS_REG(0x130400), ((uint64_t)0x00482f04 << 32) | 0xa);
    qtest_writeq(t, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484f04 << 32) | (GL_LIST_WIDGET << 16));
    gl_call_list(t, 0, 17);
    g_assert_cmphex(mgras_test_pixel(t, 320, 320), ==, 0xffff00);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_list_page_on_another_node(void)
{
    gl_list_page_on_node(1);
}

/*
 * The node field is all eight bits of the SN0 node number, entry bits
 * 27:20: the same list on a peer numbered 255, the highest the Hub
 * routes, draws yellow.
 */
static void test_gl_list_page_on_the_highest_node(void)
{
    gl_list_page_on_node(255);
}

/*
 * A next range at pool offset 0 is a range like any other. The probe's
 * list with its ranges at 0x3000, 0x0000, 0x1000 and 0x2000: the first
 * header names offset 0, which the client's pool reuses once lists are
 * freed. The window is the probe's.
 */
static void test_gl_list_continues_at_offset_zero(void)
{
    static const uint32_t header[4][3] = {
        { GL_LIST_NEXT_HDR, 0x0000, 0x3f9 },
        { GL_LIST_NEXT_HDR, 0x1000, 0x3f9 },
        { GL_LIST_NEXT_HDR, 0x2000, 0x121 }, { GL_LIST_LAST_HDR, 0 },
    };
    static const uint32_t offset[] = { 0x3000, 0x0000, 0x1000, 0x2000 };
    static const unsigned int first[] = { 0, 78, 156, 234, 256 };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 4);
    for (int i = 0; i < 4; i++) {
        gl_list_page(qts, offset[i], header[i], i < 3 ? 3 : 2,
                     gl_list_triangle, first[i], first[i + 1] - first[i],
                     i == 0, i == 3);
    }
    gl_call_list(qts, 0x3000, 0x3fa);
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_window_sha256(rgb, GL_LIST_SHA256);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A list that calls a list. The client ends a range at a glCallList with
 * the six-word header 0x00083708, the next range, 0x00080c08 and the
 * called list's first range (the range-header routine, the display-list
 * call-resolve routine). The probe's triangles 0-77 are in the calling range at
 * 0x0000, 78-155 in a one-range list at 0x2000 that it calls, and 156-255 in
 * the calling list's next two ranges at 0x1000 and 0x3000. Run in glCallList's
 * order the words draw the probe's window, twice over from one execute each.
 */
static void test_gl_list_calls_a_list(void)
{
    const uint32_t call[6] = { GL_LIST_CALL_HDR, 0x1000, 0x3f9,
                               GL_LIST_NEXT_HDR, 0x2000, 0x3f8 };
    static const uint32_t next[3] = { GL_LIST_NEXT_HDR, 0x3000, 0x121 };
    static const uint32_t last[2] = { GL_LIST_LAST_HDR, 0 };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 4);
    g_assert_cmpuint(gl_list_page(qts, 0x0000, call, 6, gl_list_triangle,
                                  0, 78, true, false), ==, 1021);
    g_assert_cmpuint(gl_list_page(qts, 0x2000, last, 2, gl_list_triangle,
                                  78, 78, false, false), ==, 0x3f8);
    g_assert_cmpuint(gl_list_page(qts, 0x1000, next, 3, gl_list_triangle,
                                  156, 78, false, false), ==, 0x3f9);
    g_assert_cmpuint(gl_list_page(qts, 0x3000, last, 2, gl_list_triangle,
                                  234, 22, false, true), ==, 0x121);
    gl_call_list(qts, 0, 1021);
    gl_call_list(qts, 0, 1021);
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_window_sha256(rgb, GL_LIST_SHA256);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Calls for the two limit tests below: pool page 0 holds 65 calling
 * ranges of header alone, 0x40 bytes apart, each calling the next and
 * returning to a last range at 0x1c00; the 65th calls a list at 0x1800
 * whose one range draws a yellow triangle over the window.
 */
#define GL_LIST_CALLERS 65
#define GL_LIST_CALLER(k) (0x40u * (k))
#define GL_LIST_LEAF 0x1800u
#define GL_LIST_RETURN 0x1c00u
#define GL_LIST_LEAF_WORDS 17

static void gl_list_leaf_and_return(QTestState *t)
{
    static const uint32_t last[2] = { GL_LIST_LAST_HDR, 0 };

    gl_list_window_triangle(t, ((uint64_t)GL_LIST_POOL_PFN(1) << 12) |
                            (GL_LIST_LEAF & 0xfff), 1, 1);
    gl_list_put(t, GL_LIST_RETURN, last, ARRAY_SIZE(last));
}

/*
 * The model follows 64 nested calls, as many as the display-list DMA execute
 * routine lets a list by DMA reach, and refuses a 65th whole. Called from the
 * second range the chain is 64 calls deep and draws; from the first it is 65
 * and draws nothing.
 */
static void test_gl_list_calls_nest_64_deep(void)
{
    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 2);
    for (unsigned int k = 0; k < GL_LIST_CALLERS; k++) {
        bool leaf = k == GL_LIST_CALLERS - 1;
        const uint32_t call[6] = {
            GL_LIST_CALL_HDR, GL_LIST_RETURN, 2, GL_LIST_NEXT_HDR,
            leaf ? GL_LIST_LEAF : GL_LIST_CALLER(k + 1),
            leaf ? GL_LIST_LEAF_WORDS : 6,
        };

        gl_list_put(qts, GL_LIST_CALLER(k), call, ARRAY_SIZE(call));
    }
    gl_list_leaf_and_return(qts);
    gl_call_list(qts, GL_LIST_CALLER(0), 6);
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0x0000ff);
    gl_call_list(qts, GL_LIST_CALLER(1), 6);
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0xffff00);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A call's fourth word must be 0x00080c08. The client could form
 * 0x00080e08 there from a flag it never sets; the model refuses it and
 * does not run the called list, which would draw the window yellow.
 */
static void test_gl_list_call_needs_its_next_token(void)
{
    const uint32_t call[6] = { GL_LIST_CALL_HDR, GL_LIST_RETURN, 2,
                               0x00080e08, GL_LIST_LEAF, GL_LIST_LEAF_WORDS };

    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 2);
    gl_list_put(qts, 0, call, ARRAY_SIZE(call));
    gl_list_leaf_and_return(qts);
    gl_call_list(qts, 0, ARRAY_SIZE(call));
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0x0000ff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A range header the client never writes draws nothing: 0x00080e08,
 * which the client could form from a flag it never sets, and a last
 * range whose second word is not zero, over a triangle that covers the
 * window.
 */
static void test_gl_list_unknown_header_draws_nothing(void)
{
    static const uint32_t header[2][3] = {
        { 0x00080e08, 0x1000, 0x20 }, { GL_LIST_LAST_HDR, 1 },
    };
    uint32_t w[] = { 0, 0, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR,
                     gl_bits(1), gl_bits(1), gl_bits(0),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(3), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(3),
                     GL_END_TRIANGLES };

    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 1);
    for (int i = 0; i < 2; i++) {
        memcpy(w, header[i], sizeof(header[i]));
        gl_list_put(qts, 0, w, ARRAY_SIZE(w));
        gl_call_list(qts, 0, ARRAY_SIZE(w));
        g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0x0000ff);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * One list whose words run from the end of pool page 0 into page 1, with
 * a glVertex2f packet split between them. The pages sit at descending
 * frame numbers, so only the page list puts the second part after the
 * first. It draws the probe's triangle case.
 */
static void test_gl_list_crosses_a_page(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { 0.75f, -0.625f }, { -0.125f, 0.734375f },
    };
    uint32_t w[17] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR,
                       gl_bits(1), gl_bits(1), gl_bits(0) };
    unsigned int n = 7;
    g_autofree uint32_t *rgb = NULL;

    for (int i = 0; i < 3; i++) {
        w[n++] = GL_VERTEX2F_HDR;
        w[n++] = gl_bits(v[i][0]);
        w[n++] = gl_bits(v[i][1]);
    }
    w[n++] = GL_END_TRIANGLES;
    g_assert_cmpuint(n, ==, ARRAY_SIZE(w));

    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 2);
    /* Twelve words on page 0: the second vertex's y is on page 1. */
    gl_list_put(qts, 0xfd0, w, n);
    gl_call_list(qts, 0xfd0, n);
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
}

/*
 * A list cut off inside a packet leaves the port's parser where it was:
 * the port's next words are a header, not the list's missing float. The
 * list sends glBegin, the colour, two vertices and the header and x of a
 * third; the port sends the whole third vertex and glEnd, and the
 * triangle is the probe's.
 */
static void test_gl_list_ends_inside_a_packet(void)
{
    uint32_t w[] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR,
                     gl_bits(1), gl_bits(1), gl_bits(0),
                     GL_VERTEX2F_HDR, gl_bits(-0.75f), gl_bits(-0.75f),
                     GL_VERTEX2F_HDR, gl_bits(0.75f), gl_bits(-0.625f),
                     GL_VERTEX2F_HDR, gl_bits(0.5f) };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_list_frame(qts);
    gl_list_install(qts, 1);
    gl_list_put(qts, 0, w, ARRAY_SIZE(w));
    gl_call_list(qts, 0, ARRAY_SIZE(w));
    gl_vertex2f(qts, -0.125f, 0.734375f);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
}

/*
 * One execute may fetch at most 2^20 words, whatever the pool holds: the
 * pool and its sizes are the guest's, so without a limit of the model's
 * own one execute could make it copy gigabytes. The pool here is 1,025
 * pages that all name one frame, which holds two header words and a
 * yellow triangle over the whole window, then zeros. A size of 2^20 + 1
 * words fits the pool and is refused, and nothing is drawn; 2^20 words
 * draw the triangle.
 */
#define GL_LIST_MAX_WORDS (1u << 20)
#define GL_LIST_MAX_POOL_PAGES 1025
#define GL_LIST_MAX_FRAME 0x2c0u

static void test_gl_list_over_the_word_limit(void)
{
    uint32_t w[] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR,
                     gl_bits(1), gl_bits(1), gl_bits(0),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(3), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(3),
                     GL_END_TRIANGLES };
    g_autofree uint8_t *entries = g_new0(uint8_t,
                                         8 * GL_LIST_MAX_POOL_PAGES);
    uint8_t bytes[sizeof(w)];

    for (int i = 0; i < GL_LIST_MAX_POOL_PAGES; i++) {
        stl_be_p(entries + 8 * i + 4, GL_LIST_MAX_FRAME);
    }
    for (int i = 0; i < ARRAY_SIZE(w); i++) {
        stl_be_p(bytes + 4 * i, w[i]);
    }

    mgras_reset();
    gl_list_frame(qts);
    qtest_memwrite(qts, GL_LIST_PAGE_LIST + 0x18, entries,
                   8 * GL_LIST_MAX_POOL_PAGES);
    qtest_memset(qts, (uint64_t)GL_LIST_MAX_FRAME << 12, 0, 4096);
    qtest_memwrite(qts, (uint64_t)GL_LIST_MAX_FRAME << 12, bytes,
                   sizeof(bytes));
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482808);
    qtest_writeq(qts, MGRAS_REG(0x130400), GL_LIST_PAGE_LIST | 0x19);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00483404 << 32) | GL_LIST_MAX_POOL_PAGES);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484f04 << 32) | (GL_LIST_WIDGET << 16));

    gl_call_list(qts, 0, GL_LIST_MAX_WORDS + 1);
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0x0000ff);
    gl_call_list(qts, 0, GL_LIST_MAX_WORDS);
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0xffff00);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * One execute may follow at most 2,048 ranges, so a guest cannot make it
 * append ranges without end: a looping chain of 3-word ranges would
 * otherwise run on until the pool's words ran out. Here each of 2,049
 * pool pages, each its own frame, holds a range of its header alone
 * naming the next page, and the last holds a yellow triangle over the
 * whole window. Called from page 0 the chain is 2,049 ranges and is
 * refused whole, so nothing is drawn; called from page 1 it is 2,048
 * and draws.
 */
#define GL_LIST_RANGES 2049
#define GL_LIST_RANGE_FRAME(i) (0x1000u + (i))

static void test_gl_list_over_the_range_limit(void)
{
    uint32_t last[] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES,
                        GL_COLOR3F_HDR, gl_bits(1),
                        gl_bits(1), gl_bits(0),
                        GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(-1),
                        GL_VERTEX2F_HDR, gl_bits(3), gl_bits(-1),
                        GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(3),
                        GL_END_TRIANGLES };
    g_autofree uint8_t *entries = g_new0(uint8_t, 8 * GL_LIST_RANGES);
    uint8_t bytes[sizeof(last)];

    for (int i = 0; i < GL_LIST_RANGES; i++) {
        uint64_t frame = (uint64_t)GL_LIST_RANGE_FRAME(i) << 12;
        uint8_t header[12];

        stl_be_p(entries + 8 * i + 4, GL_LIST_RANGE_FRAME(i));
        if (i < GL_LIST_RANGES - 1) {
            stl_be_p(header, GL_LIST_NEXT_HDR);
            stl_be_p(header + 4, (i + 1) << 12);
            stl_be_p(header + 8, i < GL_LIST_RANGES - 2 ? 3 :
                     ARRAY_SIZE(last));
            qtest_memwrite(qts, frame, header, sizeof(header));
        }
    }
    for (int i = 0; i < ARRAY_SIZE(last); i++) {
        stl_be_p(bytes + 4 * i, last[i]);
    }

    mgras_reset();
    gl_list_frame(qts);
    qtest_memwrite(qts, GL_LIST_PAGE_LIST + 0x18, entries,
                   8 * GL_LIST_RANGES);
    qtest_memwrite(qts, (uint64_t)GL_LIST_RANGE_FRAME(GL_LIST_RANGES - 1)
                   << 12, bytes, sizeof(bytes));
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482808);
    qtest_writeq(qts, MGRAS_REG(0x130400), GL_LIST_PAGE_LIST | 0x19);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00483404 << 32) | GL_LIST_RANGES);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484f04 << 32) | (GL_LIST_WIDGET << 16));

    gl_call_list(qts, 0, 3);
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0x0000ff);
    gl_call_list(qts, 0x1000, 3);
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0xffff00);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The word limit covers the whole chain, not only the range the execute
 * names. Pool page 0 holds a 3-word range naming a second range at
 * 0x1000 of n words; pages 1 to 1,024 all name one frame holding two
 * header words that end the chain and a yellow triangle over the whole
 * window. With n = 2^20 - 2 the chain asks for 2^20 + 1 words and is
 * refused, drawing nothing; with n = 2^20 - 3 it asks for exactly 2^20
 * and draws.
 */
static void gl_list_chain_of(uint32_t n)
{
    uint8_t header[12];

    stl_be_p(header, GL_LIST_NEXT_HDR);
    stl_be_p(header + 4, 0x1000);
    stl_be_p(header + 8, n);
    qtest_memwrite(qts, (uint64_t)GL_LIST_RANGE_FRAME(0) << 12, header,
                   sizeof(header));
    gl_call_list(qts, 0, 3);
}

static void test_gl_list_chain_over_the_word_limit(void)
{
    uint32_t w[] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR,
                     gl_bits(1), gl_bits(1), gl_bits(0),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(3), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(3),
                     GL_END_TRIANGLES };
    g_autofree uint8_t *entries = g_new0(uint8_t,
                                         8 * GL_LIST_MAX_POOL_PAGES);
    uint8_t bytes[sizeof(w)];

    stl_be_p(entries + 4, GL_LIST_RANGE_FRAME(0));
    for (int i = 1; i < GL_LIST_MAX_POOL_PAGES; i++) {
        stl_be_p(entries + 8 * i + 4, GL_LIST_MAX_FRAME);
    }
    for (int i = 0; i < ARRAY_SIZE(w); i++) {
        stl_be_p(bytes + 4 * i, w[i]);
    }

    mgras_reset();
    gl_list_frame(qts);
    qtest_memwrite(qts, GL_LIST_PAGE_LIST + 0x18, entries,
                   8 * GL_LIST_MAX_POOL_PAGES);
    qtest_memset(qts, (uint64_t)GL_LIST_MAX_FRAME << 12, 0, 4096);
    qtest_memwrite(qts, (uint64_t)GL_LIST_MAX_FRAME << 12, bytes,
                   sizeof(bytes));
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482808);
    qtest_writeq(qts, MGRAS_REG(0x130400), GL_LIST_PAGE_LIST | 0x19);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00483404 << 32) | GL_LIST_MAX_POOL_PAGES);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484f04 << 32) | (GL_LIST_WIDGET << 16));

    gl_list_chain_of(GL_LIST_MAX_WORDS - 2);
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0x0000ff);
    gl_list_chain_of(GL_LIST_MAX_WORDS - 3);
    g_assert_cmphex(mgras_test_pixel(qts, 320, 320), ==, 0xffff00);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The DMA-read fence marker 0x000e0100, which the client also pads
 * pooled lists with, is consumed without a word of log: on the port it
 * neither sets HQ flag 0x10000 nor disturbs the framing of the words
 * after it, and in a list it logs nothing either.
 */
static void test_gl_dma_read_fence_is_consumed_quietly(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { 0.75f, -0.625f }, { -0.125f, 0.734375f },
    };
    uint32_t w[] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES, 0x000e0100,
                     GL_END_TRIANGLES, 0x000e0100, 0x000e0100 };
    g_autofree char *log_path = NULL;
    g_autofree char *args = NULL;
    g_autofree char *log = NULL;
    g_autofree uint32_t *rgb = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *t;
    int fd;

    fd = g_file_open_tmp("sgi-mgras-fence-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    close(fd);
    args = g_strdup_printf(MACHINE_ARGS " -d unimp,guest_errors -D %s",
                           log_path);
    t = qtest_init(args);

    gl_list_frame(t);
    gl_list_install(t, 1);
    gl_list_put(t, 0, w, ARRAY_SIZE(w));
    gl_call_list(t, 0, ARRAY_SIZE(w));
    qtest_writel(t, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    gl_word(t, 0x000e0100);
    g_assert_cmphex(qtest_readl(t, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    gl_color3f(t, 1, 1, 0);
    gl_word(t, GL_BEGIN_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        gl_word(t, 0x000e0100);
        gl_vertex2f(t, v[i][0], v[i][1]);
    }
    gl_word(t, GL_END_TRIANGLES);
    gl_word(t, GL_FLUSH);
    rgb = mgras_screendump(t);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
    qtest_quit(t);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    unlink(log_path);
    g_assert_null(strstr(log, "class 0x000e"));
}

/*
 * An execute whose words do not all lie in the pool fails whole: nothing
 * is drawn, the fetch reports SGI_FAIL_BOUNDS (2), and the port goes on.
 * So does a chain that names itself, once it has asked for more words
 * than the pool holds. A size near 2^32 words draws nothing either; the
 * word limit refuses it before any fetch, so it leaves no fetch record.
 */
static void test_gl_list_past_the_pool_is_bounded(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { 0.75f, -0.625f }, { -0.125f, 0.734375f },
    };
    uint32_t w[] = { GL_LIST_LAST_HDR, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR,
                     gl_bits(1), gl_bits(0), gl_bits(0),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(1), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(0), gl_bits(1),
                     GL_END_TRIANGLES };
    g_autofree char *trace_path = NULL;
    g_autofree char *args = NULL;
    g_autofree char *trace = NULL;
    g_autofree uint32_t *rgb = NULL;
    static const uint32_t loop[] = { GL_LIST_NEXT_HDR, 0x800, 3 };
    g_auto(GStrv) failed = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *t;
    int fd;

    fd = g_file_open_tmp("sgi-mgras-list-XXXXXX.trace", &trace_path, &error);
    g_assert_no_error(error);
    close(fd);
    args = g_strdup_printf(MACHINE_ARGS " -trace enable=sgi_fetch -D %s",
                           trace_path);
    t = qtest_init(args);

    gl_list_frame(t);
    gl_list_install(t, 1);
    /* A red triangle over the whole window, if anything drew it. */
    gl_list_put(t, 0xfbc, w, ARRAY_SIZE(w));
    gl_call_list(t, 0xfbc, ARRAY_SIZE(w) + 1);
    gl_call_list(t, 0x1000, 1);
    gl_call_list(t, 0, 0xffffffffu);
    /* A range whose header names itself as the next. */
    gl_list_put(t, 0x800, loop, ARRAY_SIZE(loop));
    gl_call_list(t, 0x800, ARRAY_SIZE(loop));
    g_assert_cmphex(mgras_test_pixel(t, 320, 320), ==, 0x0000ff);

    gl_word(t, GL_SHADE_MODEL_HDR);
    gl_word(t, GL_FLAT);
    gl_color3f(t, 1, 1, 0);
    gl_word(t, GL_BEGIN_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        gl_vertex2f(t, v[i][0], v[i][1]);
    }
    gl_word(t, GL_END_TRIANGLES);
    gl_word(t, GL_FLUSH);
    rgb = mgras_screendump(t);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);

    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, &error));
    g_assert_no_error(error);
    unlink(trace_path);
    failed = g_strsplit(trace, "status 2\n", -1);
    g_assert_cmpuint(g_strv_length(failed), ==, 4);
    g_assert_null(strstr(trace, "status 0\n"));
}
#define GL_MOVE_RASTER_HDR    0x0000d608

/*
 * A 10x2 image with its corner at the raster position (10.5, 20.5) less
 * the origin (2, 3): pixel (8, 17) (OpenGL 1.2.1, 3.7). Its word 0x804001ff
 * is the rows 0x80 0x40 and 0x01 0xff, the bottom row first and the most
 * significant bit leftmost, so it sets (8, 17) and (17, 17) and (15, 18),
 * (16, 18) and (17, 18), the rest of the last byte lying beyond the
 * width, in the raster colour. The raster position then moves by (12,
 * -1.5), and 0xd608 moves it again. The zero word after the block parses
 * as a header: the fence after it sets its flag.
 */
static void test_gl_bitmap_draws_its_image(void)
{
    static const struct {
        int x, y;
    } set[] = { { 8, 17 }, { 17, 17 }, { 15, 18 }, { 16, 18 }, { 17, 18 } };
    static const uint32_t image[1] = { 0x804001ff };
    unsigned drawn = 0;

    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 0.5f, 0);
    gl_raster_pos2f(qts, 10.5f / 64 - 1, 20.5f / 64 - 1);
    gl_bitmap_packet(qts, 10, 2, 2, 3, 12, -1.5f);
    gl_bitmap_image(qts, image, 1);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);

    for (int y = 14; y < 22; y++) {
        for (int x = 4; x < 22; x++) {
            bool on = false;

            for (int i = 0; i < ARRAY_SIZE(set); i++) {
                on |= set[i].x == x && set[i].y == y;
            }
            g_assert_cmphex(gl_window_pixel(qts, x, y), ==,
                            on ? 0xff8000 : 0);
            drawn += on;
        }
    }
    g_assert_cmpuint(drawn, ==, ARRAY_SIZE(set));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(22.5f + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_Y, 1), ==,
                    gl_bits(19 + GL_RASTER_BIAS));

    gl_word(qts, GL_MOVE_RASTER_HDR);
    gl_float(qts, -2);
    gl_float(qts, 4);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(20.5f + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_Y, 1), ==,
                    gl_bits(23 + GL_RASTER_BIAS));
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * An image not word aligned: its two bytes 0xc0 0x03 start at byte 2 of
 * the first word, which the block header's bits 23:22 give, and the
 * block is one word longer than its length. The client sends no zero
 * word after it, since two words is an even count. The 16x1 image sets
 * columns 0, 1, 14 and 15. The fence as the block's second word shows the
 * extra word did not parse as a header, and a block after no bitmap is
 * framed the same way. With a negative width, or with the raster
 * position invalid, the same image draws nothing and the position does
 * not move (OpenGL 1.2.1, 3.7).
 */
static void test_gl_bitmap_unaligned_image(void)
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 1);
    gl_raster_pos2f(qts, 4.5f / 64 - 1, 6.5f / 64 - 1);
    gl_bitmap_packet(qts, 16, 1, 0, 0, 0, 0);
    gl_word(qts, GL_INLINE_BLOCK | 2 << 22 | 4);
    gl_word(qts, 0x1234c003);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    for (int x = 0; x < 20; x++) {
        bool on = x == 4 || x == 5 || x == 18 || x == 19;

        g_assert_cmphex(gl_window_pixel(qts, x, 6), ==, on ? 0xffffff : 0);
        g_assert_cmphex(gl_window_pixel(qts, x, 7), ==, 0);
    }

    /* A block no packet owns is framed the same way. */
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    gl_word(qts, GL_INLINE_BLOCK | 3 << 22 | 4);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);

    /*
     * A negative width is ignored: its image is consumed and not drawn,
     * and the raster position does not move.
     */
    gl_raster_pos2f(qts, 30.5f / 64 - 1, 6.5f / 64 - 1);
    gl_word(qts, GL_BITMAP_HDR);
    gl_word(qts, 0x1800f);
    gl_word(qts, -16);
    gl_word(qts, 1);
    gl_float(qts, 0);
    gl_float(qts, 0);
    gl_float(qts, 5);
    gl_float(qts, 5);
    gl_word(qts, 1);
    gl_word(qts, GL_INLINE_BLOCK | 4);
    gl_word(qts, 0xffff0000);
    gl_word(qts, 0);
    g_assert_cmphex(gl_window_pixel(qts, 30, 6), ==, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(30.5f + GL_RASTER_BIAS));
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    /* Outside the view volume: the image is consumed and not drawn. */
    gl_raster_pos2f(qts, 2, 0);
    gl_bitmap_packet(qts, 16, 1, 0, 0, 3, 3);
    gl_word(qts, GL_INLINE_BLOCK | 4);
    gl_word(qts, 0xffff0000);
    gl_word(qts, 0);
    g_assert_cmphex(gl_window_pixel(qts, 6, 6), ==, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(30.5f + GL_RASTER_BIAS));
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A context switch inside a bitmap's image: the second context's words
 * parse as its own, and the first context's image continues where it
 * stopped when it is loaded again, drawing its second word's rows.
 */
static void test_gl_bitmap_split_by_a_context_switch(void)
{
    mgras_reset();
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_one(qts, GL_INIT_CI_HDR, 8);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_indexs(qts, 5);
    gl_raster_pos2f(qts, 20, 30);
    gl_bitmap_packet(qts, 16, 4, 0, 0, 0, 0);
    gl_word(qts, GL_INLINE_BLOCK | 8);
    gl_word(qts, 0xffff0000);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, 0x0000ffff);
    for (int x = 20; x < 36; x++) {
        g_assert_cmphex(gl_window_pixel(qts, x, 30), ==, 5);
        g_assert_cmphex(gl_window_pixel(qts, x, 31), ==, 0);
        g_assert_cmphex(gl_window_pixel(qts, x, 32), ==, 0);
        g_assert_cmphex(gl_window_pixel(qts, x, 33), ==, 5);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}
#define GL_HQ_PIXEL_FORMAT    0x000c0004
#define GL_HQ_PIXEL_ABGR      0xe8d
#define GL_HQ_PIXEL_REST      0xe00

static void gl_pixels_context(QTestState *qts, uint32_t index_bits)
{
    mgras_reset();
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    if (index_bits) {
        gl_one(qts, GL_INIT_CI_HDR, index_bits);
    }
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
}

/*
 * mandel's rectwrite: colour indices, two bytes each, big-endian, inline
 * one block a row, the bottom row first. Three indices a row take two
 * words; the second row's block starts at byte 2 of its first word, which
 * bits 23:22 give, and counts that word (its length is the words sent).
 * An index keeps its low eight bits, the context's mask.
 */
static void test_gl_pixels_inline_colour_index(void)
{
    gl_pixels_context(qts, 8);
    gl_pixels_program(qts, GL_PIX_INDEX, 6, 10, 20, 12, 21, 1);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x01230045, 0x00670000 }, 2);
    gl_pixels_row(qts, 2, (const uint32_t[]){ 0xdead0089, 0x00ab00cd }, 2);
    gl_pixels_fence(qts);

    g_assert_cmphex(gl_window_pixel(qts, 10, 20), ==, 0x23);
    g_assert_cmphex(gl_window_pixel(qts, 11, 20), ==, 0x45);
    g_assert_cmphex(gl_window_pixel(qts, 12, 20), ==, 0x67);
    g_assert_cmphex(gl_window_pixel(qts, 10, 21), ==, 0x89);
    g_assert_cmphex(gl_window_pixel(qts, 11, 21), ==, 0xab);
    g_assert_cmphex(gl_window_pixel(qts, 12, 21), ==, 0xcd);
    g_assert_cmphex(gl_window_pixel(qts, 13, 20), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 10, 22), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 9, 21), ==, 0);
    g_assert_cmphex(mgras_test_format(qts, 256 + 10, 383 - 20), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A colour-index image into the back buffer lands in bank 1 as CI8
 * pixels, as a colour-index draw does there, and bank 0 keeps its own.
 */
static void test_gl_pixels_colour_index_into_bank_one(void)
{
    gl_pixels_context(qts, 8);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    mgras_write_cmap_entry(qts, 29, 5, 0xff000000);
    mgras_write_cmap_entry(qts, 29, 0x13, 0x0000ff00);
    gl_draw_buffer(qts, 2);
    gl_pixels_program(qts, GL_PIX_INDEX, 6, 10, 20, 12, 20, 1);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x00050013, 0x00050000 }, 2);
    gl_pixels_fence(qts);

    g_assert_cmphex(gl_window_pixel(qts, 11, 20), ==, 0);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 10, 383 - 20), ==,
                    0xff0000);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 11, 383 - 20), ==,
                    0x0000ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 12, 383 - 20), ==,
                    0xff0000);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * imgview's image inline: GL_RGB, three bytes a pixel, so a pixel runs
 * across a word boundary. Five pixels a row are 15 bytes in four words;
 * the last byte is padding.
 */
static void test_gl_pixels_inline_rgb(void)
{
    gl_pixels_context(qts, 0);
    gl_pixels_program(qts, GL_PIX_3UB, 15, 30, 40, 34, 40, 1);
    gl_pixels_draw(qts, 4);
    gl_pixels_row(qts, 0, (const uint32_t[]){
        0x10203011, 0x21311222, 0x32132333, 0x142434ff }, 4);
    gl_pixels_fence(qts);

    g_assert_cmphex(gl_window_pixel(qts, 30, 40), ==, 0x102030);
    g_assert_cmphex(gl_window_pixel(qts, 31, 40), ==, 0x112131);
    g_assert_cmphex(gl_window_pixel(qts, 32, 40), ==, 0x122232);
    g_assert_cmphex(gl_window_pixel(qts, 33, 40), ==, 0x132333);
    g_assert_cmphex(gl_window_pixel(qts, 34, 40), ==, 0x142434);
    g_assert_cmphex(gl_window_pixel(qts, 35, 40), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * ipaste's GL_ABGR_EXT image by DMA: bytes alpha, blue, green, red, which
 * the HQ pixel-format word 0xe8d marks, three pixels a row and rows 16
 * bytes apart, the bottom row first. The raster engine's own rectangle
 * from before is not painted: the transfer is the GE's.
 */
static void test_gl_pixels_dma_abgr(void)
{
    static const uint8_t image[32] = {
        0xff, 0x03, 0x02, 0x01, 0xff, 0x06, 0x05, 0x04,
        0xff, 0x09, 0x08, 0x07, 0xee, 0xee, 0xee, 0xee,
        0xff, 0x13, 0x12, 0x11, 0xff, 0x16, 0x15, 0x14,
        0xff, 0x19, 0x18, 0x17, 0xee, 0xee, 0xee, 0xee,
    };

    gl_pixels_context(qts, 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (400u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (402u << 16) | 401u));
    gl_pixels_program(qts, GL_PIX_4UB, 12, 5, 7, 7, 8, 1);
    gl_one(qts, GL_HQ_PIXEL_FORMAT, GL_HQ_PIXEL_ABGR);
    gl_pixels_draw_as(qts, 3, GL_PIXELS_BY_DMA);
    gl_pixels_dma(qts, image, sizeof(image), 16, 2, 12, 0xa7);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    gl_one(qts, GL_HQ_PIXEL_FORMAT, GL_HQ_PIXEL_REST);

    g_assert_cmphex(gl_window_pixel(qts, 5, 7), ==, 0x010203);
    g_assert_cmphex(gl_window_pixel(qts, 6, 7), ==, 0x040506);
    g_assert_cmphex(gl_window_pixel(qts, 7, 7), ==, 0x070809);
    g_assert_cmphex(gl_window_pixel(qts, 5, 8), ==, 0x111213);
    g_assert_cmphex(gl_window_pixel(qts, 7, 8), ==, 0x171819);
    g_assert_cmphex(gl_window_pixel(qts, 8, 7), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 5, 9), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 400, 400), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 402, 401), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Without the ABGR bit a four-byte image is GL_RGBA (inferred: no capture
 * has one), and imgview's GL_RGB image by DMA takes three bytes a pixel.
 */
static void test_gl_pixels_dma_rgba_and_rgb(void)
{
    static const uint8_t rgba[8] = {
        0x01, 0x02, 0x03, 0xff, 0x04, 0x05, 0x06, 0xff,
    };
    static const uint8_t rgb[6] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 };

    gl_pixels_context(qts, 0);
    gl_pixels_program(qts, GL_PIX_4UB, 8, 50, 60, 51, 60, 1);
    gl_pixels_draw_as(qts, 2, GL_PIXELS_BY_DMA);
    gl_pixels_dma(qts, rgba, sizeof(rgba), 8, 1, 8, 0xa7);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0x010203);
    g_assert_cmphex(gl_window_pixel(qts, 51, 60), ==, 0x040506);

    gl_pixels_program(qts, GL_PIX_3UB, 6, 50, 61, 51, 61, 1);
    gl_pixels_draw_as(qts, 2, GL_PIXELS_BY_DMA);
    gl_pixels_dma(qts, rgb, sizeof(rgb), 6, 1, 6, 0xa7);
    g_assert_cmphex(gl_window_pixel(qts, 50, 61), ==, 0x212223);
    g_assert_cmphex(gl_window_pixel(qts, 51, 61), ==, 0x242526);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * imgview's GL_RGB image on the SSI: 0x8d20 gives each row's words in two
 * parts, 48 and 48 for 128 pixels of three bytes, and a tile whose rows
 * lie back to back in host memory comes as one transfer row. Here eight
 * pixels a row are 6 words, given as 3 and 3, and the two rows come first
 * as two transfer rows, then as one of 48 bytes. Both draw the whole
 * image.
 */
static void test_gl_pixels_dma_rows_in_two_parts(void)
{
    uint8_t rgb[48];

    for (int i = 0; i < 16; i++) {
        rgb[3 * i] = 0x10 + i;
        rgb[3 * i + 1] = 0x40 + i;
        rgb[3 * i + 2] = 0x80 + i;
    }
    gl_pixels_context(qts, 0);
    gl_pixels_program(qts, GL_PIX_3UB, 24, 50, 60, 57, 61, 1);
    gl_pixels_draw_parts(qts, 3, 3, 0x88);
    gl_pixels_dma(qts, rgb, sizeof(rgb), 24, 2, 24, 0xa7);
    for (int i = 0; i < 16; i++) {
        g_assert_cmphex(gl_window_pixel(qts, 50 + i % 8, 60 + i / 8), ==,
                        (0x10 + i) << 16 | (0x40 + i) << 8 | (0x80 + i));
    }

    gl_pixels_program(qts, GL_PIX_3UB, 24, 50, 62, 57, 63, 1);
    gl_pixels_draw_parts(qts, 3, 3, 0x88);
    gl_pixels_dma(qts, rgb, sizeof(rgb), 48, 1, 48, 0xa7);
    for (int i = 0; i < 16; i++) {
        g_assert_cmphex(gl_window_pixel(qts, 50 + i % 8, 62 + i / 8), ==,
                        (0x10 + i) << 16 | (0x40 + i) << 8 | (0x80 + i));
    }
    g_assert_cmphex(gl_window_pixel(qts, 58, 62), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 50, 64), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A program the model does not know (a routine between the head and the
 * unpack, as a scale or bias would add, or a known program whose unpack
 * word is another's) and a pixel zoom other than 1 draw nothing, and
 * their images are consumed: the fence after each parses as a header,
 * and a DMA transfer paints neither the window nor
 * the raster engine's rectangle. A DMA transfer that does not fit a draw
 * moves nothing either, and the draw ends.
 */
static void test_gl_pixels_unmodelled_draw_nothing(void)
{
    static const uint8_t image[8] = {
        0x01, 0x02, 0x03, 0xff, 0x04, 0x05, 0x06, 0xff,
    };

    gl_pixels_context(qts, 0);
    gl_pixels_program(qts, GL_PIX_SCALED, 8, 50, 60, 51, 60, 1);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x010203ff, 0x040506ff }, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0);

    gl_pixels_program(qts, GL_PIX_4UB, 8, 50, 60, 51, 60, 2);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x010203ff, 0x040506ff }, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0);

    /* The four-byte program whose unpack word is the three-byte one's. */
    gl_pixels_program(qts, GL_PIX_4UB_AS_3, 8, 50, 60, 51, 60, 1);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x010203ff, 0x040506ff }, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0);

    /* A refused draw's DMA transfer moves nothing, into either engine. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (400u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (401u << 16) | 400u));
    gl_pixels_program(qts, GL_PIX_SCALED, 8, 50, 60, 51, 60, 1);
    gl_pixels_draw_as(qts, 2, GL_PIXELS_BY_DMA);
    gl_pixels_dma(qts, image, sizeof(image), 8, 1, 8, 0xa7);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 400, 400), ==, 0);

    gl_pixels_program(qts, GL_PIX_4UB, 8, 50, 60, 51, 60, 1);
    gl_pixels_draw_as(qts, 2, GL_PIXELS_BY_DMA);
    gl_pixels_dma(qts, image, sizeof(image), 8, 1, 4, 0xa7);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0);
    gl_pixels_fence(qts);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The rectangle is the image's own; the draw lands at the raster
 * position. At (10.75, 20.25) pixel 0 of the bottom row covers x 10.75 to
 * 11.75 and y 20.25 to 21.25, whose centres are (11.5, 20.5): window pixel
 * (11, 20) (OpenGL 1.2.1, 3.6.4). mandel's rows all come at 0, 0 this way.
 * With the raster position culled, the image is consumed and nothing is
 * drawn.
 */
static void test_gl_pixels_follow_the_raster_position(void)
{
    gl_pixels_context(qts, 8);
    gl_raster_pos2f(qts, 10.75f, 20.25f);
    gl_pixels_program(qts, GL_PIX_INDEX, 6, 0, 0, 2, 0, 1);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x00010002, 0x00030000 }, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 10, 20), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 11, 20), ==, 1);
    g_assert_cmphex(gl_window_pixel(qts, 13, 20), ==, 3);
    g_assert_cmphex(gl_window_pixel(qts, 11, 21), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 0, 0), ==, 0);

    gl_raster_pos2f(qts, 30, 200);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x00040005, 0x00060000 }, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 11, 20), ==, 1);
    g_assert_cmphex(gl_window_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * An inline row belongs to the draw only when it follows the draw's
 * 0x8d20, or the row before it. A draw 0x8d20 marks as DMA (0x99) takes
 * no inline row at all, and its DMA transfer still draws; an inline draw
 * (0x18) takes no block that follows another packet, here a raster
 * register pair's, while the rows after a later 0x8d20 draw.
 */
static void test_gl_pixels_rows_follow_their_draw(void)
{
    static const uint8_t image[8] = {
        0x01, 0x02, 0x03, 0xff, 0x04, 0x05, 0x06, 0xff,
    };
    static const uint32_t row[2] = { 0x0a0b0cff, 0x0d0e0fff };

    gl_pixels_context(qts, 0);
    gl_pixels_program(qts, GL_PIX_4UB, 8, 50, 60, 51, 60, 1);
    gl_pixels_draw_as(qts, 2, GL_PIXELS_BY_DMA);
    gl_pixels_row(qts, 0, row, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0);
    gl_pixels_dma(qts, image, sizeof(image), 8, 1, 8, 0xa7);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0x010203);

    gl_pixels_draw(qts, 2);
    gl_word(qts, 0x00007f08);
    gl_word(qts, 1);
    gl_word(qts, 2);
    gl_word(qts, GL_INLINE_BLOCK | 8);
    gl_word(qts, 0x156);
    gl_word(qts, 0xffffffff);
    gl_pixels_row(qts, 0, row, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0x010203);

    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, row, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0x0a0b0c);
    g_assert_cmphex(gl_window_pixel(qts, 51, 60), ==, 0x0d0e0f);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Only a kick with bit 4 clear feeds a waiting draw. A raster-engine
 * kick (0xb7, as the X server's) while one waits paints the raster
 * engine's rectangle as before, and the draw still takes the 0xa7
 * transfer after it.
 */
static void test_gl_pixels_raster_kick_leaves_the_draw(void)
{
    static const uint8_t image[8] = {
        0x01, 0x02, 0x03, 0xff, 0x04, 0x05, 0x06, 0xff,
    };

    gl_pixels_context(qts, 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (400u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (401u << 16) | 400u));
    gl_pixels_program(qts, GL_PIX_4UB, 8, 50, 60, 51, 60, 1);
    gl_pixels_draw_as(qts, 2, GL_PIXELS_BY_DMA);
    gl_pixels_dma(qts, image, sizeof(image), 8, 1, 8, 0xb7);
    g_assert_cmphex(mgras_test_pixel(qts, 400, 400), !=, 0);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0);
    gl_pixels_dma(qts, image, sizeof(image), 8, 1, 8, 0xa7);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0x010203);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A raster position and rectangle as far out as a guest can put them: a
 * viewport 2^31 - 1 pixels wide from x 2^30 puts the position about
 * 2.7 * 10^9 pixels out, past what an int32 holds, and a rectangle at
 * x 0x7ffffffe is past it again. The draw
 * lands off the window, draws nothing and leaves the port framed; so
 * does the same far to the left.
 */
static void test_gl_pixels_far_raster_position(void)
{
    static const uint32_t row[2] = { 0x010203ff, 0x040506ff };

    gl_pixels_context(qts, 0);
    gl_viewport(qts, 0x40000000, 0, 0x7fffffff, 128);
    gl_raster_pos2f(qts, 100, 64);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_VALID, 1), ==, UINT32_MAX);
    gl_pixels_program(qts, GL_PIX_4UB, 8, 0x7ffffffe, 0x7ffffffe,
                      0x7fffffff, 0x7ffffffe, 1);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, row, 2);
    gl_pixels_fence(qts);

    gl_viewport(qts, 0x80000001, 0x80000001, 0x7fffffff, 0x7fffffff);
    gl_raster_pos2f(qts, 100, 64);
    gl_pixels_program(qts, GL_PIX_4UB, 8, 0x80000000, 0x80000000,
                      0x80000001, 0x80000000, 1);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, row, 2);
    gl_pixels_fence(qts);

    for (int y = 0; y < 128; y += 7) {
        for (int x = 0; x < 128; x += 7) {
            g_assert_cmphex(gl_window_pixel(qts, x, y), ==, 0);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A load from 0x100 bytes on is the header with no length and an inline
 * block giving it. A 60-word program load that way is stored like a short
 * one: the draw after it reads its program.
 */
static void test_gl_pixels_long_load(void)
{
    uint32_t words[60] = { 0 };

    gl_pixels_context(qts, 8);
    gl_pixels_program(qts, GL_PIX_4UB, 12, 70, 80, 72, 80, 1);
    words[0] = 0x1a3e;
    words[2] = 0x352d;
    words[3] = 0x1d6;
    gl_ge_load(qts, GL_GE_CRAM, GL_GE_PROGRAM, words, 60);
    gl_pixels_fence(qts);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x00110022, 0x00330000 }, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 70, 80), ==, 0x11);
    g_assert_cmphex(gl_window_pixel(qts, 72, 80), ==, 0x33);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A context switch between rows: the other context's words parse as its
 * own and the first draw continues where it stopped.
 */
static void test_gl_pixels_follow_the_context(void)
{
    gl_pixels_context(qts, 8);
    gl_pixels_program(qts, GL_PIX_INDEX, 6, 90, 90, 92, 91, 1);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x00050005, 0x00050000 }, 2);
    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x0, 0x0 }, 2);
    gl_pixels_fence(qts);
    gl_enter_slot(qts, GL_SLOT_A);
    gl_pixels_row(qts, 0, (const uint32_t[]){ 0x00060006, 0x00060000 }, 2);
    gl_pixels_fence(qts);
    g_assert_cmphex(gl_window_pixel(qts, 90, 90), ==, 5);
    g_assert_cmphex(gl_window_pixel(qts, 92, 91), ==, 6);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The ERAM write routine's form for the image at offset 0: two chunks, the end.
 */
static void gl_eram_write(QTestState *qts, uint32_t addr)
{
    for (uint32_t i = 0; i < 2; i++) {
        gl_one(qts, GL_ERAM_COUNT_HDR, GL_EVICT_CHUNK);
        gl_eram_kick(qts, i * GL_EVICT_CHUNK * 4, GL_EVICT_CHUNK * 4,
                     GL_ERAM_WRITE_KICK);
        gl_word(qts, GL_ERAM_WRITE_HDR);
        gl_word(qts, addr + i * GL_EVICT_CHUNK);
        gl_word(qts, GL_EVICT_CHUNK);
    }
    gl_word(qts, GL_ERAM_END);
}

/*
 * a is read out and written back under b's address, where its state, not
 * b's, then draws: the clear fills the window in red. A new context under
 * a's old address starts from OpenGL's initial state, not a's.
 */
static void test_gl_eviction_moves_the_context(void)
{
    mgras_reset();
    gl_eviction_contexts(qts);

    gl_enter_slot(qts, GL_SLOT_DEFAULT);
    gl_eram_map(qts);
    gl_eram_read(qts, GL_EVICT_FROM);

    mgras_enter_gl_image(qts, GL_EVICT_FROM, 0xc0000000, 256, 640, 128, 128,
                         0x11);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 100, 100), ==, 0);

    gl_enter_slot(qts, GL_SLOT_DEFAULT);
    gl_eram_write(qts, GL_EVICT_TO);
    gl_enter_slot(qts, GL_EVICT_TO);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 2, 2), ==, 0xff0000);
    g_assert_cmphex(gl_window_pixel(qts, 100, 100), ==, 0xff0000);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A context read out to the buffer at host address 0 (node 0, frame 0)
 * is still tracked as saved: the write-back from the same buffer moves
 * it, as in the test above, and b does not keep its own state.
 */
static void test_gl_eviction_buffer_at_host_zero(void)
{
    mgras_reset();
    gl_eviction_contexts(qts);

    gl_enter_slot(qts, GL_SLOT_DEFAULT);
    gl_eram_map_from(qts, 0, 0);
    gl_eram_read(qts, GL_EVICT_FROM);

    gl_enter_slot(qts, GL_SLOT_DEFAULT);
    gl_eram_write(qts, GL_EVICT_TO);
    gl_enter_slot(qts, GL_EVICT_TO);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 2, 2), ==, 0xff0000);
    g_assert_cmphex(gl_window_pixel(qts, 100, 100), ==, 0xff0000);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A read kick that any header but 0xfc08 follows is not a read-out: the
 * later 0xfc08 saves nothing, the write-back finds no context saved at
 * its host address, and b keeps its own state.
 */
static void test_gl_eviction_needs_the_read_token(void)
{
    mgras_reset();
    gl_eviction_contexts(qts);

    gl_enter_slot(qts, GL_SLOT_DEFAULT);
    gl_eram_map(qts);
    gl_eram_kick(qts, 0, GL_EVICT_WORDS * 4, GL_ERAM_READ_KICK);
    gl_word(qts, GL_FLUSH);
    gl_word(qts, GL_ERAM_READ_HDR);
    gl_word(qts, GL_EVICT_FROM);
    gl_word(qts, GL_EVICT_WORDS);
    gl_eram_write(qts, GL_EVICT_TO);

    gl_enter_slot(qts, GL_EVICT_TO);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 2, 2), ==, 0x00ff00);
    g_assert_cmphex(gl_window_pixel(qts, 100, 100), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The model's own log of an ERAM transfer it could not place: these
 * tests count what it says as the only visible difference, since a
 * chunk the model misreads changes no GL state.
 */
static char *gl_eviction_log_start(QTestState **t, const char *args)
{
    g_autoptr(GError) error = NULL;
    char *log_path = NULL;
    g_autofree char *full = NULL;
    int fd = g_file_open_tmp("sgi-mgras-eram-XXXXXX.log", &log_path, &error);

    g_assert_no_error(error);
    close(fd);
    full = g_strdup_printf("%s -d unimp,guest_errors -D %s", args, log_path);
    *t = qtest_init(full);
    return log_path;
}

static char *gl_eviction_log_end(QTestState *t, char *log_path)
{
    g_autoptr(GError) error = NULL;
    char *log = NULL;

    qtest_quit(t);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    unlink(log_path);
    g_free(log_path);
    return log;
}

/*
 * A write-back's later chunk follows the last in ERAM but need not in
 * host memory: here the second chunk comes from the list's fourth page,
 * not the one after the first chunk's. The model continues by ERAM
 * address and logs nothing; the context still moves.
 */
static void test_gl_eviction_chunks_need_not_be_adjacent(void)
{
    g_autofree char *log = NULL;
    QTestState *t;
    char *path = gl_eviction_log_start(&t, MACHINE_ARGS);

    gl_eviction_contexts(t);
    gl_enter_slot(t, GL_SLOT_DEFAULT);
    gl_eram_map(t);
    gl_eram_read(t, GL_EVICT_FROM);

    gl_enter_slot(t, GL_SLOT_DEFAULT);
    gl_one(t, GL_ERAM_COUNT_HDR, GL_EVICT_CHUNK);
    gl_eram_kick(t, 0, GL_EVICT_CHUNK * 4, GL_ERAM_WRITE_KICK);
    gl_word(t, GL_ERAM_WRITE_HDR);
    gl_word(t, GL_EVICT_TO);
    gl_word(t, GL_EVICT_CHUNK);
    gl_one(t, GL_ERAM_COUNT_HDR, GL_EVICT_CHUNK);
    gl_eram_kick(t, 0x3000, GL_EVICT_CHUNK * 4, GL_ERAM_WRITE_KICK);
    gl_word(t, GL_ERAM_WRITE_HDR);
    gl_word(t, GL_EVICT_TO + GL_EVICT_CHUNK);
    gl_word(t, GL_EVICT_CHUNK);
    gl_word(t, GL_ERAM_END);

    gl_enter_slot(t, GL_EVICT_TO);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(t, 100, 100), ==, 0xff0000);
    log = gl_eviction_log_end(t, path);
    g_assert_null(strstr(log, "saw no context"));
    g_assert_null(strstr(log, "no data before"));
}

/*
 * A few words go to ERAM inline: 0xfd04 with count 1, an 8-byte inline
 * block, then 0xfb08 with the address (a captured run).
 * No context image moves, so b keeps its state and nothing is logged.
 */
static void test_gl_eviction_inline_write_moves_nothing(void)
{
    g_autofree char *log = NULL;
    QTestState *t;
    char *path = gl_eviction_log_start(&t, MACHINE_ARGS);

    gl_eviction_contexts(t);
    gl_enter_slot(t, GL_SLOT_DEFAULT);
    gl_one(t, GL_ERAM_COUNT_HDR, 1);
    gl_word(t, 0x80000008);
    gl_word(t, 0x12345678);
    gl_word(t, 0);
    gl_word(t, GL_ERAM_WRITE_HDR);
    gl_word(t, 0xb26);
    gl_word(t, 1);
    gl_word(t, GL_ERAM_END);

    gl_enter_slot(t, GL_EVICT_TO);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(t, 2, 2), ==, 0x00ff00);
    g_assert_cmphex(gl_window_pixel(t, 100, 100), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    log = gl_eviction_log_end(t, path);
    g_assert_null(strstr(log, "no data"));
    g_assert_null(strstr(log, "no DMA"));
    g_assert_null(strstr(log, "saw no context"));
}

/*
 * The host buffer is a node-qualified address: the same frames on node 1
 * and node 0 are different buffers. Context a is read out into node
 * read_node's pages and written back from node write_node's; the read
 * and write-back match only when the nodes do. Returns the pixel the
 * moved context's clear leaves under b's scissor.
 */
static uint32_t gl_eviction_across_nodes(unsigned int read_node,
                                         unsigned int write_node)
{
    QTestState *t = qtest_init(GL_LIST_NODE_ARGS);
    uint32_t pixel;

    sn0_program_nasid(t, 1);
    gl_eviction_contexts(t);
    gl_enter_slot(t, GL_SLOT_DEFAULT);
    gl_eram_map_node(t, read_node);
    gl_eram_read(t, GL_EVICT_FROM);
    gl_enter_slot(t, GL_SLOT_DEFAULT);
    gl_eram_map_node(t, write_node);
    gl_eram_write(t, GL_EVICT_TO);
    gl_enter_slot(t, GL_EVICT_TO);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    pixel = gl_window_pixel(t, 100, 100);
    qtest_quit(t);
    return pixel;
}

static void test_gl_eviction_buffer_on_another_node(void)
{
    /* Same node: a's state comes back, red outside b's old scissor. */
    g_assert_cmphex(gl_eviction_across_nodes(1, 1), ==, 0xff0000);
    /* Another node's frames are not the buffer a went to: b is as it was. */
    g_assert_cmphex(gl_eviction_across_nodes(1, 0), ==, 0);
    g_assert_cmphex(gl_eviction_across_nodes(0, 1), ==, 0);
}

/*
 * The X server's host read through the raster engine's data port
 * (the pixel-read routine in the IRIX 6.5.30 impactsr.so): raster registers by
 * 8-byte direct commands on the main port, an IR_ALIAS execute, then each
 * 8-byte unit read as 0x02d1c0 and 0x02c1c4. The box is in hardware rows,
 * counted up from the bottom of the screen.
 */
#define REG_RE_DATA_HI        MGRAS_REG(0x02d1c0)
#define REG_RE_DATA_LO        MGRAS_REG(0x02c1c4)
#define PIO_SCREEN_HEIGHT     1024

static void rss_direct(QTestState *qts, uint32_t reg, uint32_t value,
                       bool execute)
{
    uint64_t word = ((uint64_t)(0x00180004 | reg << 8) << 32) | value;

    if (execute) {
        word |= 0x0004000000000000ULL;
    }
    qtest_writeq(qts, REG_CONTEXT_COMMAND, word);
}

/* Read screen columns x to x + w - 1 of rows y to y + h - 1. */
static void pio_read_start(QTestState *qts, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint32_t mode)
{
    uint32_t top = PIO_SCREEN_HEIGHT - 1 - y;

    rss_direct(qts, 0x112, 0xca4, false);
    rss_direct(qts, 0x115, 0, false);
    rss_direct(qts, 0x160, 2, false);
    rss_direct(qts, 0x161, 0x500, false);
    rss_direct(qts, 0x046, x << 16 | top, false);
    rss_direct(qts, 0x047, (x + w - 1) << 16 | (top - h + 1), false);
    rss_direct(qts, 0x153, h << 16 | w, false);
    rss_direct(qts, 0x158, h << 16 | w, false);
    rss_direct(qts, 0x159, mode, false);
    rss_direct(qts, 0x110, 0x800000, false);
    rss_direct(qts, 0x045, 0x18, false);
    rss_direct(qts, 0x102, 0xa, false);
    rss_direct(qts, 0x00c, 1, false);
    rss_direct(qts, 0x045, 0x18, true);
}

/* Paint screen pixel (x, y) through the public port. */
static void pio_paint(QTestState *qts, uint32_t x, uint32_t y,
                      uint32_t packed)
{
    mgras_public_write(qts, 0x5b, packed, false);
    mgras_public_rect(qts, x, y, 1, 1, true);
}

/*
 * Two pixels, one unit: the first word is the left pixel's and the second
 * the right's, each with red in bits 7:0 as the DMA read gives them.
 */
static void test_pio_read_returns_the_box(void)
{
    mgras_reset();
    mgras_public_setup(qts, false);
    pio_paint(qts, 640, 512, 0x00332211);
    pio_paint(qts, 641, 512, 0x00665544);
    mgras_enter_context(qts);

    pio_read_start(qts, 640, 512, 2, 1, 0x8000);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x332211);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0x665544);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Three columns by two rows: each row starts a unit, the third column's
 * unit carrying zero in its second word, and the top row comes first.
 */
static void test_pio_read_rows_start_a_unit(void)
{
    mgras_reset();
    mgras_public_setup(qts, false);
    for (uint32_t i = 0; i < 6; i++) {
        pio_paint(qts, 100 + i % 3, 200 + i / 3, 0x00010101 * (i + 1));
    }
    mgras_enter_context(qts);

    pio_read_start(qts, 100, 200, 3, 2, 0x8000);
    for (uint32_t row = 0; row < 2; row++) {
        uint32_t first = 0x010101 * (3 * row + 1);

        g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, first);
        g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==,
                        first + 0x010101);
        g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==,
                        first + 0x020202);
        g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The server's transfers to the screen through the port set XFRCONTROL 2,
 * without the read bit, and execute IR_ALIAS the same way. Such an
 * execute is not a read: the read under way keeps its next unit.
 */
static void test_pio_write_control_starts_no_read(void)
{
    mgras_reset();
    mgras_public_setup(qts, false);
    for (uint32_t i = 0; i < 4; i++) {
        pio_paint(qts, 640 + i, 512, 0x00010101 * (i + 1));
    }
    mgras_enter_context(qts);

    pio_read_start(qts, 640, 512, 4, 1, 0x8000);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x010101);
    rss_direct(qts, 0x102, 0x2, false);
    rss_direct(qts, 0x159, 0x10000, false);
    rss_direct(qts, 0x045, 0x18, true);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x030303);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0x040404);
}

/* A transfer mode no capture has shown reads zero rather than pixels. */
static void test_pio_read_other_mode_reads_zero(void)
{
    mgras_reset();
    mgras_public_setup(qts, false);
    pio_paint(qts, 640, 512, 0x00332211);
    mgras_enter_context(qts);

    pio_read_start(qts, 640, 512, 2, 1, 0x1c000);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0);
}

/*
 * glReadPixels and glCopyPixels as the IRIX 6.5.30 OpenGL client sends
 * them (see "Pixel read-back and copy" in the GL command contract).
 * No capture has the board's side of a read, so these tests pin the
 * model's approximations: the register records reach the raster engine,
 * 0x8f08 starts a data-port read, the bracket words are consumed, and a
 * slow copy is a read followed by an ordinary draw.
 */
#define GL_RSS_PAIRS_HDR      0x00007f08
#define GL_RSS_TRIPLES_HDR    0x00007c08
#define GL_READ_OPEN          0x00009600
#define GL_READ_CLOSE         0x00009700
#define GL_READ_SETUP_HDR     0x0000db04
#define GL_READ_PIO_HDR       0x00008f08
#define GL_SAVE_RSS_HDR       0x0000d214
#define GL_RESTORE_RSS        0x0000d300
#define GL_RSS_XFRSIZE        0x153
#define GL_RSS_XFRCOUNTERS    0x158
#define GL_RSS_XFRMODE        0x159
#define GL_READ_MODE_RGBA     0x80

typedef struct GLRecord {
    uint32_t reg;
    uint32_t value;
} GLRecord;

/* Raster register pairs: record and word counts, then an inline block. */
static void gl_rss_pairs(QTestState *qts, const GLRecord *r, uint32_t n)
{
    gl_word(qts, GL_RSS_PAIRS_HDR);
    gl_word(qts, n);
    gl_word(qts, 2 * n);
    gl_word(qts, GL_INLINE_BLOCK | 8 * n);
    for (uint32_t i = 0; i < n; i++) {
        gl_word(qts, r[i].reg);
        gl_word(qts, r[i].value);
    }
}

/* Raster register records: (register, value, shadow address) triples. */
static void gl_rss_triples(QTestState *qts, const GLRecord *r, uint32_t n)
{
    uint32_t words = (3 * n + 1) & ~1u;

    gl_word(qts, GL_RSS_TRIPLES_HDR);
    gl_word(qts, n);
    gl_word(qts, words);
    gl_word(qts, GL_INLINE_BLOCK | 4 * words);
    for (uint32_t i = 0; i < n; i++) {
        gl_word(qts, r[i].reg);
        gl_word(qts, r[i].value);
        gl_word(qts, 0x2b6);
    }
    if (words != 3 * n) {
        gl_word(qts, 0);
    }
}

/*
 * The five registers every read writes, for a box of w by h pixels at
 * screen column x whose hardware rows run y0 to y1 (the client's start is
 * its low row, the X server's its high one), in transfer mode mode.
 */
static void gl_read_registers(QTestState *qts, uint32_t x, uint32_t y0,
                              uint32_t y1, uint32_t w, uint32_t h,
                              uint32_t mode, bool triples)
{
    const GLRecord r[] = {
        { GL_RSS_XFRSIZE, h << 16 | w },
        { GL_RSS_XFRCOUNTERS, h << 16 | w },
        { GL_RSS_XFRMODE, mode },
        { REG_BLOCK_START, x << 16 | y0 },
        { REG_BLOCK_END, (x + w - 1) << 16 | y1 },
    };

    if (triples) {
        gl_rss_triples(qts, r, ARRAY_SIZE(r));
    } else {
        gl_rss_pairs(qts, r, ARRAY_SIZE(r));
    }
}

/* The client's data-port read: setup word, then the 0x8f08 trigger. */
static void gl_read_setup_word(QTestState *qts)
{
    gl_one(qts, GL_READ_SETUP_HDR, 0x1);
}

static void gl_read_trigger(QTestState *qts)
{
    gl_word(qts, GL_READ_PIO_HDR);
    gl_word(qts, 0);
    gl_word(qts, 0);
}

/* A 4-byte RGBA image of the given pixel words, drawn inline at (x, y). */
static void gl_draw_rgba_row(QTestState *qts, uint32_t x, uint32_t y,
                             const uint32_t *px, uint32_t n)
{
    gl_pixels_program(qts, GL_PIX_4UB, 4 * n, x, y, x + n - 1, y, 1);
    gl_pixels_draw(qts, n);
    gl_pixels_row(qts, 0, px, n);
    gl_pixels_fence(qts);
}

/* Hardware row of window row y in the probe window gl_enter_slot() makes. */
#define GL_READ_HW_ROW(y)     (640 + (y))

/* The client's two-pixel read of the window's row y. */
static void gl_client_read_row(QTestState *qts, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h)
{
    gl_word(qts, GL_READ_OPEN);
    gl_read_registers(qts, 256 + x, GL_READ_HW_ROW(y),
                      GL_READ_HW_ROW(y) + h - 1, w, h, GL_READ_MODE_RGBA,
                      false);
    gl_read_setup_word(qts);
    gl_read_trigger(qts);
}

/*
 * The 0x7f08 and 0x7c08 records reach the raster engine's registers: the
 * X server's data-port read, set up by records rather than by direct
 * writes, returns the same units, and a record that moves the box moves
 * the result.
 */
static void test_gl_records_apply_the_read_registers(void)
{
    /* A register the model does not apply, after the five it does. */
    const GLRecord extra[] = { { 0x156, 0xffffffff } };

    mgras_reset();
    mgras_public_setup(qts, false);
    pio_paint(qts, 640, 512, 0x00332211);
    pio_paint(qts, 641, 512, 0x00665544);
    mgras_enter_context(qts);

    for (int triples = 0; triples < 2; triples++) {
        /* Hardware row 511 is screen row 512 of the 1024-row screen. */
        gl_read_registers(qts, 640, 511, 511, 2, 1, 0x8000, triples);
        rss_direct(qts, 0x102, 0xa, false);
        rss_direct(qts, 0x045, 0x18, true);
        g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x332211);
        g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0x665544);
    }

    /* Records that move the box one column right move the result. */
    gl_read_registers(qts, 641, 511, 511, 2, 1, 0x8000, false);
    rss_direct(qts, 0x045, 0x18, true);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x665544);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0);

    /* One the model does not apply changes nothing. */
    gl_read_registers(qts, 640, 511, 511, 2, 1, 0x8000, false);
    gl_rss_pairs(qts, extra, ARRAY_SIZE(extra));
    rss_direct(qts, 0x045, 0x18, true);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x332211);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0x665544);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * glReadPixels' data-port form for RGBA: records, 0xdb04 and a word, then
 * 0x8f08 and two zeros. Without the 0x8f08 nothing is read; with it the
 * window's pixels come back as units, red in bits 7:0, the box's top row
 * first (approximated: the client's own row order is not captured).
 */
static void test_gl_readpixels_dataport_rgba(void)
{
    static const uint32_t row0[2] = { 0x010203ff, 0x040506ff };
    static const uint32_t row1[2] = { 0x111213ff, 0x141516ff };

    gl_pixels_context(qts, 0);
    gl_draw_rgba_row(qts, 10, 20, row0, 2);
    gl_draw_rgba_row(qts, 10, 21, row1, 2);
    g_assert_cmphex(gl_window_pixel(qts, 11, 21), ==, 0x141516);

    /* No trigger: the polls and the setup alone start nothing. */
    gl_word(qts, GL_READ_OPEN);
    gl_read_registers(qts, 256 + 10, GL_READ_HW_ROW(20), GL_READ_HW_ROW(21),
                      2, 2, GL_READ_MODE_RGBA, false);
    gl_read_setup_word(qts);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0);

    gl_read_trigger(qts);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x131211);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0x161514);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x030201);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0x060504);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0);
    gl_word(qts, GL_READ_CLOSE);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

#define GL_READ_SYNC          0x0000f200
#define HQ_STATUS_SYNC        0x1

/*
 * The client's poll of HQ status bit 0: six reads back to back, then the
 * same again without bound. Six reads that miss it are a client that
 * never returns.
 */
static bool hq_sync_seen(QTestState *qts)
{
    for (int i = 0; i < 6; i++) {
        if (qtest_readl(qts, REG_HQ_STATUS) & HQ_STATUS_SYNC) {
            return true;
        }
    }
    return false;
}

/*
 * Both of the client's pixel reads wait for HQ status bit 0 before they
 * go on: a DMA read into a buffer that is not 8-byte aligned after its
 * 0xf200, and a data-port read after its 0x8f08. glReadPixels in the SGI
 * pandora exerciser spun forever on the first while bit 0 read clear.
 */
static void test_gl_readpixels_sync_bit_set(void)
{
    static const uint32_t row0[2] = { 0x010203ff, 0x040506ff };

    gl_pixels_context(qts, 0);
    gl_draw_rgba_row(qts, 10, 20, row0, 2);

    gl_word(qts, GL_READ_OPEN);
    gl_word(qts, GL_READ_SYNC);
    g_assert_true(hq_sync_seen(qts));
    gl_word(qts, GL_READ_CLOSE);

    gl_client_read_row(qts, 10, 20, 2, 1);
    g_assert_true(hq_sync_seen(qts));
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x030201);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0x060504);
    gl_word(qts, GL_READ_CLOSE);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/* The transfer modes the model has no evidence for read zero, logged. */
static void test_gl_readpixels_other_mode_reads_zero(void)
{
    static const uint32_t row0[2] = { 0x010203ff, 0x040506ff };

    gl_pixels_context(qts, 0);
    gl_draw_rgba_row(qts, 10, 20, row0, 2);
    gl_word(qts, GL_READ_OPEN);
    gl_read_registers(qts, 256 + 10, GL_READ_HW_ROW(20), GL_READ_HW_ROW(20),
                      2, 1, 0x01, false);
    gl_read_setup_word(qts);
    gl_read_trigger(qts);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0);
}

/*
 * Everything a read brackets itself with is consumed without a log line
 * and without disturbing the next draw: 0x9600 and 0x9700, the save and
 * restore words, and the ERAM parameter push.
 */
static void test_gl_readpixels_bracket_opaque(void)
{
    static const uint32_t row0[2] = { 0x010203ff, 0x040506ff };
    static const uint32_t row1[2] = { 0x717273ff, 0x747576ff };
    static const uint32_t eram[4] = { 0x18d, 0, 0, 0 };
    g_autofree char *log = NULL;
    QTestState *t;
    char *path = gl_eviction_log_start(&t, MACHINE_ARGS);

    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    gl_draw_rgba_row(t, 10, 20, row0, 2);

    gl_word(t, GL_READ_OPEN);
    gl_word(t, GL_SAVE_RSS_HDR);
    gl_word(t, 0x11e23929);
    gl_word(t, 0x01000000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 2);
    gl_ge_load(t, GL_GE_ERAM, 0x18d, eram, ARRAY_SIZE(eram));
    gl_client_read_row(t, 10, 20, 2, 1);
    g_assert_cmphex(qtest_readl(t, REG_RE_DATA_HI), ==, 0x030201);
    g_assert_cmphex(qtest_readl(t, REG_RE_DATA_LO), ==, 0x060504);
    gl_word(t, GL_RESTORE_RSS);
    gl_word(t, GL_READ_CLOSE);

    /* The next draw lands where it would have without the read. */
    gl_draw_rgba_row(t, 30, 40, row1, 2);
    g_assert_cmphex(gl_window_pixel(t, 30, 40), ==, 0x717273);
    g_assert_cmphex(gl_window_pixel(t, 31, 40), ==, 0x747576);
    g_assert_cmphex(gl_window_pixel(t, 10, 20), ==, 0x010203);
    g_assert_cmphex(gl_window_pixel(t, 32, 40), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    log = gl_eviction_log_end(t, path);
    g_assert_null(strstr(log, "unimplemented command-FIFO token"));
    g_assert_null(strstr(log, "ERAM"));
}

/*
 * glCopyPixels' slow path is a read followed by the client's ordinary
 * glDrawPixels at the raster position: read the source box through the
 * data port, send its pixels back as an RGBA image at the destination,
 * and read the destination box back to see the same units.
 */
static void test_gl_copypixels_slow_path(void)
{
    static const uint32_t src[3] = { 0x010203ff, 0x040506ff, 0x070809ff };
    uint32_t unit[4], px[3];

    gl_pixels_context(qts, 0);
    gl_draw_rgba_row(qts, 10, 20, src, 3);

    gl_client_read_row(qts, 10, 20, 3, 1);
    for (int i = 0; i < 4; i++) {
        unit[i] = qtest_readl(qts, i % 2 ? REG_RE_DATA_LO : REG_RE_DATA_HI);
    }
    gl_word(qts, GL_READ_CLOSE);
    g_assert_cmphex(unit[0], ==, 0x030201);
    g_assert_cmphex(unit[1], ==, 0x060504);
    g_assert_cmphex(unit[2], ==, 0x090807);
    /* The read draws nothing and moves nothing. */
    g_assert_cmphex(gl_window_pixel(qts, 10, 20), ==, 0x010203);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0);

    for (int i = 0; i < 3; i++) {
        uint32_t w = unit[i];

        px[i] = (w & 0xff) << 24 | (w >> 8 & 0xff) << 16 |
                (w >> 16 & 0xff) << 8 | 0xff;
    }
    gl_draw_rgba_row(qts, 50, 60, px, 3);
    g_assert_cmphex(gl_window_pixel(qts, 50, 60), ==, 0x010203);
    g_assert_cmphex(gl_window_pixel(qts, 51, 60), ==, 0x040506);
    g_assert_cmphex(gl_window_pixel(qts, 52, 60), ==, 0x070809);
    g_assert_cmphex(gl_window_pixel(qts, 53, 60), ==, 0);

    gl_client_read_row(qts, 50, 60, 3, 1);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x030201);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_LO), ==, 0x060504);
    g_assert_cmphex(qtest_readl(qts, REG_RE_DATA_HI), ==, 0x090807);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A DMA descriptor with a row length no draw can have, or no rows, while
 * a pixel draw waits: the transfer is refused or empty, the board does
 * not abort, and nothing is drawn. The oversized one is logged as the
 * guest's error; the empty one moves nothing.
 */
static char *gl_pixels_dma_refused(QTestState **t, uint32_t rows,
                                   uint32_t bpr)
{
    static const uint8_t image[8] = {
        0x01, 0x02, 0x03, 0xff, 0x04, 0x05, 0x06, 0xff,
    };
    char *path = gl_eviction_log_start(t, MACHINE_ARGS);

    gl_enter_slot(*t, GL_SLOT_A);
    gl_word(*t, GL_CONTEXT_OPENER);
    gl_viewport(*t, 0, 0, 128, 128);
    gl_window_projection(*t);
    gl_pixels_program(*t, GL_PIX_4UB, 8, 50, 60, 51, 60, 1);
    gl_pixels_draw_as(*t, 2, GL_PIXELS_BY_DMA);
    gl_pixels_dma(*t, image, sizeof(image), 8, rows, bpr, 0xa7);
    g_assert_cmphex(qtest_readl(*t, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(gl_window_pixel(*t, 50, 60), ==, 0);
    return path;
}

static void test_gl_pixels_dma_oversized_row_is_refused(void)
{
    g_autofree char *log = NULL;
    QTestState *t;
    char *path = gl_pixels_dma_refused(&t, 1, 0xffffffffu);

    log = gl_eviction_log_end(t, path);
    g_assert_nonnull(strstr(log, "sgi-mgras: DMA of 1 rows of 4294967295"));
}

static void test_gl_pixels_dma_no_rows_is_empty(void)
{
    g_autofree char *log = NULL;
    QTestState *t;
    char *path = gl_pixels_dma_refused(&t, 0, 8);

    log = gl_eviction_log_end(t, path);
    g_assert_null(strstr(log, "runs past"));
}

static void gl_pixels_captured_rgba_setup(QTestState *qts, bool two_rows)
{
    static const uint32_t unpack_one_row[6] = {
        0xc, 0x4064, 0x464, 0, 8, 0x3b808081,
    };
    static const uint32_t unpack_two_rows[6] = {
        0x14, 0x4064, 0x464, 0, 0x10, 0x3b808081,
    };

    gl_pixels_context(qts, 0);
    gl_pixels_program(qts, GL_PIX_4UB, two_rows ? 0x14 : 0xc,
                      0, 0, 1, two_rows ? 1 : 0, 1);
    gl_ge_load(qts, GL_GE_ERAM, GL_GE_UNPACK,
               two_rows ? unpack_two_rows : unpack_one_row,
               ARRAY_SIZE(unpack_one_row));
}

static void test_gl_pixels_ordinary_rgba_two_by_two(void)
{
    static const uint32_t row0[2] = { 0x404000ff, 0xbf4000ff };
    static const uint32_t row1[2] = { 0x40bf00ff, 0xbfbf00ff };

    gl_pixels_captured_rgba_setup(qts, true);
    gl_pixels_draw(qts, 4);
    gl_pixels_row(qts, 0, row0, ARRAY_SIZE(row0));
    gl_pixels_row(qts, 0, row1, ARRAY_SIZE(row1));
    gl_pixels_fence(qts);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 0, 0), ==, 0x404000);
    g_assert_cmphex(gl_window_pixel(qts, 1, 0), ==, 0xbf4000);
    g_assert_cmphex(gl_window_pixel(qts, 0, 1), ==, 0x40bf00);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xbfbf00);
}

static void test_gl_pixels_ordinary_rgba_single_row(void)
{
    static const uint32_t row[2] = { 0x404000ff, 0xbf4000ff };

    gl_pixels_captured_rgba_setup(qts, false);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, row, ARRAY_SIZE(row));
    gl_pixels_fence(qts);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 0, 0), ==, 0x404000);
    g_assert_cmphex(gl_window_pixel(qts, 1, 0), ==, 0xbf4000);
}

static void test_gl_pixels_ordinary_rgba_two_rows_short_count(void)
{
    static const uint32_t row0[2] = { 0x404000ff, 0xbf4000ff };
    static const uint32_t row1[2] = { 0x40bf00ff, 0xbfbf00ff };

    gl_pixels_captured_rgba_setup(qts, true);
    gl_pixels_draw(qts, 2);
    gl_pixels_row(qts, 0, row0, ARRAY_SIZE(row0));
    gl_pixels_row(qts, 0, row1, ARRAY_SIZE(row1));
    gl_pixels_fence(qts);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 1, 0), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 0, 1), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);
}

static void test_gl_pixels_rgb_count_stays_per_row(void)
{
    static const uint32_t row0[2] = { 0x11223344, 0x55660000 };
    static const uint32_t row1[2] = { 0x778899aa, 0xbbcc0000 };

    gl_pixels_context(qts, 0);
    gl_pixels_program(qts, GL_PIX_3UB, 6, 0, 0, 1, 1, 1);
    gl_pixels_draw(qts, 4);
    gl_pixels_row(qts, 0, row0, ARRAY_SIZE(row0));
    gl_pixels_row(qts, 0, row1, ARRAY_SIZE(row1));
    gl_pixels_fence(qts);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 1, 0), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 0, 1), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_ARGS);


    qtest_add_func("sn0/origin2000/mgras/dma-map-slot-registers",
                   test_dma_map_slot_registers);
    qtest_add_func("sn0/origin2000/mgras/dma-map-close-and-discard",
                   test_dma_map_close_marker_and_discard);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-paints-from-host-memory",
                   test_dma_image_transfer_paints_from_host_memory);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-traces-source-chunks",
                   test_dma_image_transfer_traces_source_chunks);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-kick-byte-selects-slot",
                   test_dma_image_transfer_kick_byte_selects_slot);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-src-off-aligned-selects-page",
                   test_dma_image_transfer_src_off_aligned_selects_page);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-reads-list-entry-at-0x18",
                   test_dma_image_transfer_reads_list_entry_at_0x18);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-past-page-list-moves-nothing",
                   test_dma_image_transfer_past_page_list_moves_nothing);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-applies-logic-op",
                   test_dma_image_transfer_applies_logic_op);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-applies-colour-mask",
                   test_dma_image_transfer_applies_colour_mask);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-draws-ci8-indices",
                   test_dma_image_transfer_draws_ci8_indices);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-lands-in-bank-zero",
                   test_dma_image_transfer_lands_in_bank_zero);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-reads-screen-to-host",
                   test_dma_image_transfer_reads_screen_to_host);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-reads-rgb-red-in-low-byte",
                   test_dma_image_transfer_reads_rgb_red_in_low_byte);
    qtest_add_func("sn0/origin2000/mgras/byte-row-pairs-rgb-survives-dma-copy",
                   test_byte_row_pairs_rgb_survives_dma_copy);
    qtest_add_func("sn0/origin2000/mgras/direct-pio-pixel-pairs",
                   test_direct_pio_pixel_pairs);
    qtest_add_func("sn0/origin2000/mgras/gl-bitmap-draws-its-image",
                   test_gl_bitmap_draws_its_image);
    qtest_add_func("sn0/origin2000/mgras/gl-bitmap-unaligned-image",
                   test_gl_bitmap_unaligned_image);
    qtest_add_func("sn0/origin2000/mgras/gl-bitmap-split-by-a-context-switch",
                   test_gl_bitmap_split_by_a_context_switch);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-inline-colour-index",
                   test_gl_pixels_inline_colour_index);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-pixels-colour-index-into-bank-one",
                   test_gl_pixels_colour_index_into_bank_one);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-inline-rgb",
                   test_gl_pixels_inline_rgb);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-ordinary-rgba-two-by-two",
                   test_gl_pixels_ordinary_rgba_two_by_two);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-ordinary-rgba-single-row",
                   test_gl_pixels_ordinary_rgba_single_row);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-ordinary-rgba-short-count",
                   test_gl_pixels_ordinary_rgba_two_rows_short_count);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-rgb-count-stays-per-row",
                   test_gl_pixels_rgb_count_stays_per_row);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-dma-abgr",
                   test_gl_pixels_dma_abgr);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-dma-rgba-and-rgb",
                   test_gl_pixels_dma_rgba_and_rgb);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-dma-rows-in-two-parts",
                   test_gl_pixels_dma_rows_in_two_parts);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-unmodelled-draw-nothing",
                   test_gl_pixels_unmodelled_draw_nothing);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-long-load",
                   test_gl_pixels_long_load);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-rows-follow-their-draw",
                   test_gl_pixels_rows_follow_their_draw);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-pixels-raster-kick-leaves-the-draw",
                   test_gl_pixels_raster_kick_leaves_the_draw);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-far-raster-position",
                   test_gl_pixels_far_raster_position);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-follow-the-raster-position",
                   test_gl_pixels_follow_the_raster_position);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-follow-the-context",
                   test_gl_pixels_follow_the_context);
    qtest_add_func("sn0/origin2000/mgras/gl-eviction-moves-the-context",
                   test_gl_eviction_moves_the_context);
    qtest_add_func("sn0/origin2000/mgras/gl-eviction-needs-the-read-token",
                   test_gl_eviction_needs_the_read_token);
    qtest_add_func("sn0/origin2000/mgras/gl-eviction-buffer-at-host-zero",
                   test_gl_eviction_buffer_at_host_zero);
    qtest_add_func("sn0/origin2000/mgras/gl-eviction-chunks-not-adjacent",
                   test_gl_eviction_chunks_need_not_be_adjacent);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-dma-oversized-row",
                   test_gl_pixels_dma_oversized_row_is_refused);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-dma-no-rows",
                   test_gl_pixels_dma_no_rows_is_empty);
    qtest_add_func("sn0/origin2000/mgras/gl-eviction-inline-write",
                   test_gl_eviction_inline_write_moves_nothing);
    qtest_add_func("sn0/origin2000/mgras/gl-eviction-buffer-on-another-node",
                   test_gl_eviction_buffer_on_another_node);
    qtest_add_func("sn0/origin2000/mgras/pio-read-returns-the-box",
                   test_pio_read_returns_the_box);
    qtest_add_func("sn0/origin2000/mgras/pio-read-rows-start-a-unit",
                   test_pio_read_rows_start_a_unit);
    qtest_add_func("sn0/origin2000/mgras/pio-read-other-mode-reads-zero",
                   test_pio_read_other_mode_reads_zero);
    qtest_add_func("sn0/origin2000/mgras/pio-write-control-starts-no-read",
                   test_pio_write_control_starts_no_read);
    qtest_add_func("sn0/origin2000/mgras/gl-list-draws-the-probe-list",
                   test_gl_list_draws_the_probe_list);
    qtest_add_func("sn0/origin2000/mgras/gl-list-follows-the-chain",
                   test_gl_list_follows_the_chain);
    qtest_add_func("sn0/origin2000/mgras/gl-list-big-matches-the-reference",
                   test_gl_list_big_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/gl-list-page-on-another-node",
                   test_gl_list_page_on_another_node);
    qtest_add_func("sn0/origin2000/mgras/gl-list-page-on-the-highest-node",
                   test_gl_list_page_on_the_highest_node);
    qtest_add_func("sn0/origin2000/mgras/gl-list-calls-nest-64-deep",
                   test_gl_list_calls_nest_64_deep);
    qtest_add_func("sn0/origin2000/mgras/gl-list-call-needs-its-next-token",
                   test_gl_list_call_needs_its_next_token);
    qtest_add_func("sn0/origin2000/mgras/gl-list-continues-at-offset-zero",
                   test_gl_list_continues_at_offset_zero);
    qtest_add_func("sn0/origin2000/mgras/gl-list-calls-a-list",
                   test_gl_list_calls_a_list);
    qtest_add_func("sn0/origin2000/mgras/gl-list-unknown-header-draws-nothing",
                   test_gl_list_unknown_header_draws_nothing);
    qtest_add_func("sn0/origin2000/mgras/gl-list-crosses-a-page",
                   test_gl_list_crosses_a_page);
    qtest_add_func("sn0/origin2000/mgras/gl-list-ends-inside-a-packet",
                   test_gl_list_ends_inside_a_packet);
    qtest_add_func("sn0/origin2000/mgras/gl-list-over-the-word-limit",
                   test_gl_list_over_the_word_limit);
    qtest_add_func("sn0/origin2000/mgras/gl-list-over-the-range-limit",
                   test_gl_list_over_the_range_limit);
    qtest_add_func("sn0/origin2000/mgras/gl-list-chain-over-the-word-limit",
                   test_gl_list_chain_over_the_word_limit);
    qtest_add_func("sn0/origin2000/mgras/gl-dma-read-fence-is-consumed-quietly",
                   test_gl_dma_read_fence_is_consumed_quietly);
    qtest_add_func("sn0/origin2000/mgras/gl-list-past-the-pool-is-bounded",
                   test_gl_list_past_the_pool_is_bounded);
    qtest_add_func("sn0/origin2000/mgras/gl-records-apply-the-read-registers",
                   test_gl_records_apply_the_read_registers);
    qtest_add_func("sn0/origin2000/mgras/gl-readpixels-dataport-rgba",
                   test_gl_readpixels_dataport_rgba);
    qtest_add_func("sn0/origin2000/mgras/gl-readpixels-sync-bit-set",
                   test_gl_readpixels_sync_bit_set);
    qtest_add_func("sn0/origin2000/mgras/gl-readpixels-other-mode-reads-zero",
                   test_gl_readpixels_other_mode_reads_zero);
    qtest_add_func("sn0/origin2000/mgras/gl-readpixels-bracket-opaque",
                   test_gl_readpixels_bracket_opaque);
    qtest_add_func("sn0/origin2000/mgras/gl-copypixels-slow-path",
                   test_gl_copypixels_slow_path);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
