/*
 * MGRAS register-contract and protocol qtests: command port, FIFO, 2D
 * raster, glyph, colour-map and scanout cases.
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


#define REG_FIFO_STATUS        MGRAS_REG(0x020008)
#define REG_GE_DATA_B           MGRAS_REG(0x058040)
#define REG_GE_CTRL_B           MGRAS_REG(0x058044)
#define REG_FIFO_THRESHOLD_0     MGRAS_REG(0x040000)
#define REG_FIFO_THRESHOLD_1     MGRAS_REG(0x040008)
#define REG_FIFO_THRESHOLD_LAST  MGRAS_REG(0x040030)
#define REG_SCHED_ARG_4         MGRAS_REG(0x040070)
#define MGRAS_PENDING_LOWATER       0x00000004
#define MGRAS_HQ_ENABLE_LOWATER     0x10


#define MGRAS_FENCE_PUSH_1          0x00080b04
#define MGRAS_FENCE_PUSH_2          0x000000b1000e0400ULL

#define REG_RED                 0x05c
#define REG_RE_LINE             0x041
#define REG_RE_BLOCKCOPY        0x045
#define REG_XYWIN               0x115

#define REG_IR_ALIAS            0x045
#define REG_ALPHA               0x05f
#define REG_GLINE_XSTARTF       0x00c
#define MGRAS_FILLMODE_GLYPH_ROW    0x10400008

/*
 * Stippled lines: FILLMODE 0x20 gates each pixel on the 0x15a pattern and
 * 0x40 paints clear bits with BKGRD_RG/BKGRD_BA; see
 * MGRAS_RSS_LINE_STIPPLE in hw/misc/sgi-mgras-raster.c.
 */
#define REG_LINE_STIPPLE              0x15a
#define MGRAS_FILLMODE_STIPPLE        0x00000020
#define MGRAS_FILLMODE_STIPPLE_OPAQUE 0x00000060
#define MGRAS_BYTE_XFER_PACKING_BYTES 0x8000


/* The full X11 GX raster-op encoding PP1FILLMODE bits 29:26 select. */
#define MGRAS_LOGIC_OP_CLEAR         0x0
#define MGRAS_LOGIC_OP_AND           0x1
#define MGRAS_LOGIC_OP_AND_REVERSE   0x2
#define MGRAS_LOGIC_OP_AND_INVERTED  0x4
#define MGRAS_LOGIC_OP_NOOP          0x5
#define MGRAS_LOGIC_OP_OR            0x7
#define MGRAS_LOGIC_OP_NOR           0x8
#define MGRAS_LOGIC_OP_EQUIV         0x9
#define MGRAS_LOGIC_OP_INVERT        0xa
#define MGRAS_LOGIC_OP_OR_REVERSE    0xb
#define MGRAS_LOGIC_OP_COPY_INVERTED 0xc
#define MGRAS_LOGIC_OP_OR_INVERTED   0xd
#define MGRAS_LOGIC_OP_NAND          0xe
#define MGRAS_LOGIC_OP_SET           0xf
#define MGRAS_PIXEL_FORMAT_RGB12   2

/*
 * The IRIX 6.5.30 server's PP1FILLMODE for a 12-bit RGB window: 0x4004 for
 * its drawing and 0xc804 for the clear that opens it.
 */
#define MGRAS_PP1FILLMODE_RGB12        0x4004
#define MGRAS_PP1FILLMODE_RGB12_CLEAR  0xc804

/*
 * Screen-to-host read: the trace's own read
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

/*
 * PACKEDCOLOR and pair-packed CHAR_H/CHAR_L carry the X server's whole
 * pixel word, red in bits 7:0, the same layout as a 4-byte DMA pixel
 * (X.Org's impact driver, OpenBSD impact(4), the Linux Impact framebuffer
 * patch). A non-grey pixel drawn through either path must therefore read
 * back by DMA as the very word that was written. Read red-first, red and
 * blue come back exchanged.
 */
static void test_rgb_pixel_words_read_back_unchanged(void)
{
    static const uint8_t list_header[24] = { 0 };
    static const uint8_t list_entry[8] = {
        0, 0, 0, 0, 0, 0, 0, DMA_READ_DATA_PFN,
    };
    static const uint8_t want[12] = {
        0x00, 0x56, 0x34, 0x12,
        0x00, 0xbc, 0x9a, 0x78,
        0x00, 0xf0, 0xe0, 0xd0,
    };
    static const uint8_t poison[12] = {
        0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
        0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
    };
    uint8_t got[12];

    mgras_reset();
    mgras_enter_context(qts);

    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_LIST_PAGE),
                  list_header, sizeof(list_header));
    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_LIST_BASE),
                  list_entry, 8);
    qtest_memwrite(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                  poison, sizeof(poison));

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, 0x0c006204));

    /* Two pair-packed pixels at (0,0) and (1,0). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BYTE_XFER_PACKING,
                           MGRAS_BYTE_XFER_PACKING_PAIRS));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_END, (1u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_CHAR_H, 0x00563412));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x00bc9a78));

    /* A PACKEDCOLOR point at (2,0). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLMODE, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00f0e0d0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_RE_POINT, (2u << 16) | 0u));

    g_assert_cmphex(mgras_test_pixel(qts, 0, 0), ==, 0x00123456);
    g_assert_cmphex(mgras_test_pixel(qts, 1, 0), ==, 0x00789abc);
    g_assert_cmphex(mgras_test_pixel(qts, 2, 0), ==, 0x00d0e0f0);

    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482008);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                dma_read_node_addr(DMA_READ_LIST_BASE));
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483004 << 32) | 1);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                ((uint64_t)0x00484b04 << 32) | (DMA_READ_WIDGET << 16));

    /* Read hw-y box (0,1023)-(2,1023): screen row 0, x 0-2, 4 bytes each. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, 0x00002200));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 1023u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (2u << 16) | 1023u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080404 << 32) | 12);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080504 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080604 << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080804 << 32) | 12);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                ((uint64_t)0x00080b04 << 32) | 0xb9);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    qtest_memread(qts, dma_read_node_addr(DMA_READ_DATA_PAGE),
                 got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), want, sizeof(want));
}

/*
 * The IRIX 6.5.30 Icon Catalog paints one colour both ways: its pane's
 * olive fill as FILLCOLOR_R/G/B 0x800/0x800/0x300 and the icons' stipple
 * over it as PACKEDCOLOR 0x308080 (a captured trace).
 * The two agree only if the PACKEDCOLOR word has red in bits 7:0.
 */
static void test_packedcolor_matches_fillcolor_channels(void)
{
    mgras_reset();
    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) |
                                               MGRAS_PP1FILLMODE_RGB));

    /* The fill at (0,0). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_R, 0x800));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_G, 0x800));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_B, 0x300));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (0u << 16) | 0u));

    /* The raw captured word, not MGRAS_PIXEL_WORD(), as a point at (1,0). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLMODE, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00308080));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_RE_POINT, (1u << 16) | 0u));

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 0, 0), ==, 0x00808030);
    g_assert_cmphex(mgras_test_pixel(qts, 1, 0), ==, 0x00808030);
}

#undef DMA_READ_NODE
#undef DMA_READ_WIDGET
#undef DMA_READ_LIST_PAGE
#undef DMA_READ_LIST_BASE
#undef DMA_READ_DATA_PFN
#undef DMA_READ_DATA_PAGE


static void test_ge_upload_and_readback(void)
{
    static const uint32_t w[6] = {
        0x11111111, 0x22222222, 0x33333333,
        0x44444444, 0x55555555, 0x66666666,
    };

    mgras_reset();

    qtest_writel(qts, REG_GE_CTRL_A, 0x200000);
    for (int i = 0; i < 6; i++) {
        qtest_writel(qts, REG_GE_DATA_A, w[i]);
    }


    qtest_writel(qts, REG_GE_CTRL_A, 0x40038);
    qtest_writel(qts, REG_GE_DATA_A, 0xd1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    qtest_writel(qts, REG_GE_CTRL_A, 0x200000);
    qtest_writel(qts, REG_GE_CTRL_A, 0x80000002);
    for (int k = 0; k < 3; k++) {
        g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, w[2 * k + 1]);
        g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, w[2 * k]);
    }


    qtest_writel(qts, REG_GE_CTRL_A, 0x200001);
    qtest_writel(qts, REG_GE_CTRL_A, 0x80000004);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, w[0]);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0);
    for (int k = 1; k < 3; k++) {
        g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, w[2 * k]);
        g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, w[2 * k - 1]);
    }

    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, w[5]);
}


static void test_ge_upload_slots_do_not_cross_contaminate(void)
{
    mgras_reset();

    qtest_writel(qts, REG_GE_CTRL_A, 0x200000);
    qtest_writel(qts, REG_GE_DATA_A, 0xaaaa0000);
    qtest_writel(qts, REG_GE_DATA_A, 0xa0a0a0a0);
    qtest_writel(qts, REG_GE_DATA_A, 0xaaaa0001);
    qtest_writel(qts, REG_GE_DATA_A, 0xa1a1a1a1);

    qtest_writel(qts, REG_GE_CTRL_B, 0x200000);
    qtest_writel(qts, REG_GE_DATA_B, 0xbbbb0000);
    qtest_writel(qts, REG_GE_DATA_B, 0xb0b0b0b0);
    qtest_writel(qts, REG_GE_DATA_B, 0xbbbb0001);
    qtest_writel(qts, REG_GE_DATA_B, 0xb1b1b1b1);
    qtest_writel(qts, REG_GE_CTRL_B, 0x80000004);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0xbbbb0000);


    qtest_writel(qts, REG_GE_CTRL_A, 0x80000004);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0xaaaa0000);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0xaaaa0001);


    qtest_writel(qts, REG_GE_CTRL_B, 0);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0xbbbb0001);
}


static void test_ge_upload_rearm_rereads_from_start(void)
{
    mgras_reset();

    qtest_writel(qts, REG_GE_CTRL_A, 0x200000);
    qtest_writel(qts, REG_GE_DATA_A, 0x01020304);
    qtest_writel(qts, REG_GE_DATA_A, 0x05060708);
    qtest_writel(qts, REG_GE_CTRL_A, 0x80000002);

    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, 0x05060708);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0x01020304);

    qtest_writel(qts, REG_GE_CTRL_A, 0x80000002);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, 0x05060708);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0x01020304);
}


static void test_prefix_register_reads_back(void)
{
    mgras_reset();

    qtest_writel(qts, REG_PREFIX, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_PREFIX), ==, 0x12345678);
}


static void test_prefix_rewrite_after_entry_selection_does_not_poison_context(void)
{
    mgras_reset();

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                     MGRAS_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);


    qtest_writel(qts, REG_PREFIX, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_PREFIX), ==, 0x12345678);

    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                     MGRAS_PENDING_CONTEXT);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}

static void test_context_recognizer_ordering(void)
{
    mgras_reset();

    mgras_enter_context(qts);
}

/*
 * A second context switch must restart the context word count and must
 * route its image words to the context store, not the command-FIFO
 * parser. Entry selection resets the acknowledgement flags and the word
 * count, but the port has been ready (able to parse command-FIFO tokens)
 * since the first switch completed, so the dispatch at 0x120400 cannot
 * gate on readiness alone or the second switch's image lands on the
 * wrong parser.
 */
static void test_second_context_switch_restarts_word_count(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    /*
     * Clear the first switch's completion bit so the assertion below can
     * only pass if the second switch raises it again -- otherwise a
     * leftover bit from the first switch masks a misrouted second image.
     */
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);

    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                     MGRAS_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);

    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }

    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                     MGRAS_PENDING_CONTEXT);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_entry_requires_prefix(void)
{
    mgras_reset();

    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ENTRY), ==, 0);
}

/*
 * The context-switch routine does not write or read a closing
 * word at 0x120400 -- the image completes on the 63rd word alone. A word
 * written afterward is ordinary command-FIFO traffic, parsed like any
 * other command token rather than checked against a fixed value.
 */
static void test_context_command_after_image_is_ordinary_token(void)
{
    mgras_reset();

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                     MGRAS_PENDING_CONTEXT);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);

    /*
     * An arbitrary zero-length header, not a recognized class or opcode:
     * falls to the command-FIFO parser's LOG_UNIMP default rather than
     * being checked against a closing-token value. Zero length so it does
     * not itself consume the fence write below as payload.
     */
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x12345600);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    /*
     * The fence token is a recognized class-0 command; it must still be
     * parsed normally as a command word after the image, not treated as
     * context data.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                     MGRAS_HQ_FLAG_FENCE);
}


static void test_fifo_occupancy_always_drained(void)
{
    mgras_reset();

    mgras_enter_context(qts);


    for (int i = 0; i < 300; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_CLIP_X, i));
        g_assert_cmphex(qtest_readl(qts, REG_FIFO_STATUS) & 0xff, ==, 0);
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_direct_fill_and_reset(void)
{
    mgras_reset();

    mgras_enter_context(qts);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x33));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));

    g_assert_cmphex(mgras_test_pixel(qts, 11, 11), ==, 0x33);
    g_assert_cmphex(mgras_test_pixel(qts, 13, 13), ==, 0x33);

    g_assert_cmphex(mgras_test_pixel(qts, 20, 11), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 9, 9), ==, 0);

    /* A colour fill leaves the WID plane where it found it. */
    g_assert_cmphex(mgras_test_wid(qts, 11, 11), ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_XOR << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));
    g_assert_cmphex(mgras_test_pixel(qts, 11, 11), ==, 0);


    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(qtest_readl(qts, REG_PENDING), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ENTRY), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 11, 11), ==, 0);
}

/*
 * PP1FILLMODE bits 29:26 select one of the sixteen X11 GX raster ops
 * (mgras_logic_op() in hw/misc/sgi-mgras-raster.c), applied as
 * op(dst, src) with dst the current pixel and src the paint colour. Fill
 * a known destination with plain COPY, then check every op against it
 * with a distinct source, against the X11 protocol's own truth table.
 */
static void test_logic_op_truth_table(void)
{
    /*
     * dst = 0x00f0f0f0, src = 0x00cccccc. Chosen so every one of the
     * sixteen X11 GX ops yields a distinct byte value (0x00, 0x0c, 0x0f,
     * 0x30, 0x33, 0x3c, 0x3f, 0xc0, 0xc3, 0xcf, 0xcc, 0xf0, 0xf3, 0xfc,
     * 0xff), so a model bug that confuses one op for another is caught
     * instead of hidden behind a coincidental match.
     */
    static const struct {
        uint8_t op;
        uint32_t expect;
    } cases[] = {
        { MGRAS_LOGIC_OP_CLEAR,         0x000000 },
        { MGRAS_LOGIC_OP_AND,           0xc0c0c0 },
        { MGRAS_LOGIC_OP_AND_REVERSE,   0x0c0c0c },
        { MGRAS_LOGIC_OP_COPY,          0xcccccc },
        { MGRAS_LOGIC_OP_AND_INVERTED,  0x303030 },
        { MGRAS_LOGIC_OP_NOOP,          0xf0f0f0 },
        { MGRAS_LOGIC_OP_XOR,           0x3c3c3c },
        { MGRAS_LOGIC_OP_OR,            0xfcfcfc },
        { MGRAS_LOGIC_OP_NOR,           0x030303 },
        { MGRAS_LOGIC_OP_EQUIV,         0xc3c3c3 },
        { MGRAS_LOGIC_OP_INVERT,        0x0f0f0f },
        { MGRAS_LOGIC_OP_OR_REVERSE,    0xcfcfcf },
        { MGRAS_LOGIC_OP_COPY_INVERTED, 0x333333 },
        { MGRAS_LOGIC_OP_OR_INVERTED,   0xf3f3f3 },
        { MGRAS_LOGIC_OP_NAND,          0x3f3f3f },
        { MGRAS_LOGIC_OP_SET,           0xffffff },
    };

    mgras_reset();

    mgras_enter_context(qts);

    /* A rect fill only paints once FILLMODE selects the rect-fill encoding. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        /* RGB format: a rect fill reads FILLCOLOR_R/G/B, each byte << 4. */
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_PP1FILLMODE,
                                (MGRAS_LOGIC_OP_COPY << 26) |
                                MGRAS_PP1FILLMODE_RGB));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_FILLCOLOR_R, 0xf0 << 4));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_FILLCOLOR_G, 0xf0 << 4));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_FILLCOLOR_B, 0xf0 << 4));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_BLOCK_START, (500u << 16) | 500u));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_BLOCK_END, (500u << 16) | 500u));
        g_assert_cmphex(mgras_test_pixel(qts, 500, 500), ==, 0x00f0f0f0);

        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_PP1FILLMODE,
                                ((uint32_t)cases[i].op << 26) |
                                MGRAS_PP1FILLMODE_RGB));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_FILLCOLOR_R, 0xcc << 4));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_FILLCOLOR_G, 0xcc << 4));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_FILLCOLOR_B, 0xcc << 4));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_BLOCK_START, (500u << 16) | 500u));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_BLOCK_END, (500u << 16) | 500u));
        g_assert_cmphex(mgras_test_pixel(qts, 500, 500), ==, cases[i].expect);
    }
}

/*
 * A GXxor rubber band's border paints its ink over the destination once
 * to show the band, then issues the identical command again on the next
 * move or on release to erase it: op(op(dst, src), src) must equal dst
 * for every primitive PP1FILLMODE reaches, or the band leaves a mark.
 * This is the rectangle-fill path (BLOCKXYSTARTI/BLOCKXYENDI).
 */
static void test_logic_op_xor_twice_restores_rect(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* A rect fill only paints once FILLMODE selects the rect-fill encoding. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));

    /* RGB format: a rect fill reads FILLCOLOR_R/G/B, each byte << 4. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) |
                            MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x65 << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_G, 0x43 << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_B, 0x21 << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (50u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (53u << 16) | 63u));
    g_assert_cmphex(mgras_test_pixel(qts, 51, 61), ==, 0x00654321);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_XOR << 26) |
                            MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xff << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_G, 0xff << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_B, 0xff << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (53u << 16) | 63u));
    g_assert_cmphex(mgras_test_pixel(qts, 51, 61), ==, 0x00654321 ^ 0x00ffffff);

    /* The rubber band's second, identical draw must restore the pixel. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (53u << 16) | 63u));
    g_assert_cmphex(mgras_test_pixel(qts, 51, 61), ==, 0x00654321);
}

/*
 * Same round trip on the RE_POINT/RE_LINE path. RE_LINE draws from
 * whatever rss[MGRAS_RSS_RE_POINT] currently holds (mgras_draw_line()),
 * so the pen is re-latched with a plain STATE write between the two XOR
 * draws: a STATE write only stores the position, it does not paint (see
 * test_re_point_execute_after_bounded_payload), so the endpoint is not
 * struck a third time.
 */
static void test_logic_op_xor_twice_restores_line(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_POINT/RE_LINE read PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) |
                            MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00123456)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (300u << 16) | 300u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (310u << 16) | 300u));
    g_assert_cmphex(mgras_test_pixel(qts, 305, 300), ==, 0x00123456);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_XOR << 26) |
                            MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00ffffff)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RE_POINT, (300u << 16) | 300u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (310u << 16) | 300u));
    g_assert_cmphex(mgras_test_pixel(qts, 305, 300), ==, 0x00123456 ^ 0x00ffffff);

    /* The rubber band's second, identical draw must restore the line. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RE_POINT, (300u << 16) | 300u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (310u << 16) | 300u));
    g_assert_cmphex(mgras_test_pixel(qts, 305, 300), ==, 0x00123456);
}

/*
 * Same round trip on the byte-transfer span path (mgras_draw_byte_row()),
 * an eight-pixel-per-execute row rather than a filled block or a line. A
 * BLOCKXYSTARTI write resets the row/unit counters (mgras_raster_rss_
 * write()), so re-arming the span between draws is a plain re-issue of
 * BLOCK_START/BLOCK_END.
 */
static void test_logic_op_xor_twice_restores_span(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BYTE_XFER_PACKING,
                            MGRAS_BYTE_XFER_PACKING_BYTES));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (400u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (407u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x10203040u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x50607080u));
    g_assert_cmphex(mgras_test_pixel(qts, 400, 400), ==, 0x10);
    g_assert_cmphex(mgras_test_pixel(qts, 404, 400), ==, 0x50);
    g_assert_cmphex(mgras_test_pixel(qts, 407, 400), ==, 0x80);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_XOR << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (400u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (407u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0xffffffffu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0xffffffffu));
    g_assert_cmphex(mgras_test_pixel(qts, 400, 400), ==, (0x10 ^ 0xff));
    g_assert_cmphex(mgras_test_pixel(qts, 404, 400), ==, (0x50 ^ 0xff));
    g_assert_cmphex(mgras_test_pixel(qts, 407, 400), ==, (0x80 ^ 0xff));

    /* The rubber band's second, identical draw must restore the span. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (400u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (407u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0xffffffffu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0xffffffffu));
    g_assert_cmphex(mgras_test_pixel(qts, 400, 400), ==, 0x10);
    g_assert_cmphex(mgras_test_pixel(qts, 404, 400), ==, 0x50);
    g_assert_cmphex(mgras_test_pixel(qts, 407, 400), ==, 0x80);
}


static void test_fill_mask_reads_colormasklsbsa(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x33));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (31u << 16) | 31u));
    g_assert_cmphex(mgras_test_pixel(qts, 30, 30), ==, 0x33);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x22));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (41u << 16) | 41u));
    g_assert_cmphex(mgras_test_pixel(qts, 40, 40), ==, 0);

    /* With COLORMASKMSBS also zero the fill writes nothing at all. */
    g_assert_cmphex(mgras_test_wid(qts, 40, 40), ==, 0);
}


/*
 * A rectangle fill reaches the WID plane when COLORMASKMSBS is nonzero,
 * taking DRBPOINTERS bits 23:16, whether or not FILLMODE also permits a
 * colour paint. Every paint with COLORMASKMSBS clear leaves the plane
 * alone, whatever DRBPOINTERS holds at the time.
 */
/*
 * A pixel's WID comes from the VC3 main display-ID table, never from a
 * drawing primitive, whatever COLORMASKMSBS and DRBPOINTERS hold.
 */
static void test_wid_from_display_id_table(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 7 << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x66));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(mgras_test_pixel(qts, 61, 61), ==, 0x66);
    g_assert_cmphex(mgras_test_wid(qts, 61, 61), ==, 0);

    /* The display-ID table sets the WID and leaves the colour alone. */
    mgras_tag_wid(qts, 60, 60, 63, 63, 7);
    g_assert_cmphex(mgras_test_wid(qts, 61, 61), ==, 7);
    g_assert_cmphex(mgras_test_pixel(qts, 61, 61), ==, 0x66);
    g_assert_cmphex(mgras_test_wid(qts, 64, 61), ==, 0);

    /* A colour-only fill leaves the WID alone. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 2 << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x33));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(mgras_test_pixel(qts, 61, 61), ==, 0x33);
    g_assert_cmphex(mgras_test_wid(qts, 61, 61), ==, 7);

    /*
     * Neither does a fill with a nonzero COLORMASKMSBS and a WID-looking
     * DRBPOINTERS; FILLMODE here also excludes the rect-fill value, so no
     * colour paints.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLMODE, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKMSBS, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 5 << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x11));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(mgras_test_pixel(qts, 61, 61), ==, 0x33);
    g_assert_cmphex(mgras_test_wid(qts, 61, 61), ==, 7);

    /*
     * Glyph mode paints per set bit, from PACKEDCOLOR under RGB, and
     * leaves the WID alone.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00010203)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_CHAR_L, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (63u << 16) | 63u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x80000000));
    g_assert_cmphex(mgras_test_pixel(qts, 60, 60), ==, 0x00010203);
    g_assert_cmphex(mgras_test_pixel(qts, 61, 60), ==, 0x33);
    g_assert_cmphex(mgras_test_wid(qts, 60, 60), ==, 7);
    g_assert_cmphex(mgras_test_wid(qts, 61, 60), ==, 7);

    /* A fill with both masks open writes the colour and no WID. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKMSBS, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 4 << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xef));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(mgras_test_pixel(qts, 61, 61), ==, 0xef);
    g_assert_cmphex(mgras_test_wid(qts, 61, 61), ==, 7);

    /* A second rectangle on the same rows keeps the first one's runs. */
    mgras_tag_wid(qts, 70, 60, 71, 63, 3);
    g_assert_cmphex(mgras_test_wid(qts, 61, 61), ==, 7);
    g_assert_cmphex(mgras_test_wid(qts, 66, 61), ==, 0);
    g_assert_cmphex(mgras_test_wid(qts, 70, 61), ==, 3);
    g_assert_cmphex(mgras_test_wid(qts, 72, 61), ==, 0);
}


/*
 * A rectangle fill (a BLOCKXYENDI execute) paints only when FILLMODE's
 * bits 22:20 read exactly the rect-fill encoding (bit 20 set, bit 22
 * clear); any other encoding, including the glyph-row encoding, leaves
 * the destination untouched when reached this way. The colour itself
 * comes from FILLCOLOR_R (CI8, masked to a CMAP index byte) rather than
 * PACKEDCOLOR, which rect fills never read.
 */
static void test_fill_color_reads_fillcolor_registers(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x66));

    /* FILLMODE 0 fails the gate: no colour paints. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLMODE, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (41u << 16) | 41u));
    g_assert_cmphex(mgras_test_pixel(qts, 40, 40), ==, 0);

    /* The exact rect-fill encoding passes the gate. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (42u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (43u << 16) | 41u));
    g_assert_cmphex(mgras_test_pixel(qts, 42, 40), ==, 0x66);

    /* Bits outside the gate mask do not disturb a passing encoding. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE,
                            MGRAS_FILLMODE_RECT_FILL_VALUE | 0x10000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (44u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (45u << 16) | 41u));
    g_assert_cmphex(mgras_test_pixel(qts, 44, 40), ==, 0x66);

    /* Bit 22 alone (the glyph-row encoding's bit) fails the gate. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (46u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (47u << 16) | 41u));
    g_assert_cmphex(mgras_test_pixel(qts, 46, 40), ==, 0);
}


/*
 * A direct-command IR_ALIAS execute paints nothing (contract rule 5): its
 * pixels arrive over the DMA and PIO paths that already paint their own
 * data, so this execute only latches whichever register write preceded
 * it. Confirm it leaves both untouched and already-painted destinations
 * exactly as it found them.
 */
static void test_ir_alias_execute_paints_nothing(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x55));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 23u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 20u));
    g_assert_cmphex(mgras_test_pixel(qts, 11, 21), ==, 0x55);

    /* An IR_ALIAS execute over an untouched region paints nothing. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (30u << 16) | 31u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (31u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(mgras_test_pixel(qts, 30, 30), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 31), ==, 0);

    /* An IR_ALIAS execute over an already-painted region leaves it be. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 23u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (13u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(mgras_test_pixel(qts, 11, 21), ==, 0x55);
    g_assert_cmphex(mgras_test_pixel(qts, 13, 20), ==, 0x55);
}


static void test_direct_glyph_row(void)
{
    static const uint8_t glyph[8] = {
        0x18, 0x24, 0x42, 0x7e, 0x42, 0x42, 0x42, 0x00,
    };

    mgras_reset();

    mgras_enter_context(qts);

    /* A glyph row reads PACKEDCOLOR only once PP1FILLMODE selects RGB. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00ffffff)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (47u << 16) | 47u));

    for (int row = 0; row < 8; row++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_CHAR_H,
                                        (uint32_t)glyph[row] << 24));
    }


    g_assert_cmphex(mgras_test_pixel(qts, 40, 40), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 43, 40), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 44, 40), ==, 0x00ffffff);

    g_assert_cmphex(mgras_test_pixel(qts, 40, 43), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 41, 43), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 47, 43), ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 40, 47), ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 50, 40), ==, 0);
}


/*
 * The IP30 PROM's graphical console draws through the main port with no
 * context switch: a fresh-NVRAM Octane SI boot writes 1,910,919 eight-byte
 * command words to 0x120400 (fills and CHAR_H glyph rows among them) and
 * never selects an entry at 0x130500. The board must draw them, or the
 * PROM's menu stays black while the PROM waits at it for a key. The IP30
 * diagnostic's bare bring-up word 0x65781 before them is not a register
 * packet and must not eat them.
 */
static void test_glyph_row_before_any_context_switch(void)
{
    mgras_reset();

    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0x65781);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00ffffff)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (47u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x81u << 24));

    g_assert_cmphex(mgras_test_pixel(qts, 40, 40), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 41, 40), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 47, 40), ==, 0x00ffffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    /* A context switch afterwards still takes its 63 image words. */
    mgras_enter_context(qts);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * One glyph row of 0x81 at (40, 40) under PP1FILLMODE pp1, with RED holding
 * index 0x30 and PACKEDCOLOR white, so the pixel shows which source and op
 * the paint took.
 */
static uint32_t mgras_glyph_row_under_pp1(uint32_t pp1)
{
    mgras_reset();
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_PP1FILLMODE, pp1));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00ffffff)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RED, 0x30u << 12));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (47u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x81u << 24));
    return mgras_test_pixel(qts, 40, 40);
}

/*
 * PP1FILLMODE bits 10:8 of 5 keep a paint CI8 although bit 13 is set: the
 * PROM console paints its indices under 0x6500. Read as RGB the row would
 * store PACKEDCOLOR's white.
 */
static void test_pp1_format_field_5_is_ci8(void)
{
    g_assert_cmphex(mgras_glyph_row_under_pp1(
                        (MGRAS_LOGIC_OP_COPY << 26) | 0x6500), ==, 0x30);
    g_assert_cmphex(mgras_test_format(qts, 40, 40), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
    /* The X server's 24-bit windows' 0x6204 still paints RGB. */
    g_assert_cmphex(mgras_glyph_row_under_pp1(
                        (MGRAS_LOGIC_OP_COPY << 26) | 0x6204), ==, 0x00ffffff);
}

/*
 * PP1FILLMODE bit 25 makes a paint replace the destination whatever the
 * op bits hold: the PROM console draws under 0x02006500, whose op bits
 * read CLEAR. Without bit 25 those bits still CLEAR.
 */
static void test_pp1_bit_25_replaces(void)
{
    g_assert_cmphex(mgras_glyph_row_under_pp1(0x02004500), ==, 0x30);
    g_assert_cmphex(mgras_glyph_row_under_pp1(
                        (MGRAS_LOGIC_OP_CLEAR << 26) | 0x4500), ==, 0);
}

/*
 * The IP30 PROM console's own drawing state, from a traced Octane SI boot:
 * PP1FILLMODE 0x02006500 (whose op bits read CLEAR), the colour as an
 * index in RED, CONFIG 4 and rows counted up from the bottom of the screen.
 */
#define REG_PROM_BLOCK               0x013
#define REG_CONFIG                   0x112
#define MGRAS_PROM_PP1FILLMODE       0x02006500
#define MGRAS_PROM_CONFIG            0x4
#define MGRAS_CONFIG_TOP_DOWN        0x8
#define MGRAS_FILLMODE_PROM_INK_FILL 0x00018000
#define MGRAS_FILLMODE_PROM_GLYPH    0x00418008
#define MGRAS_SCREEN_HEIGHT          1024

static void mgras_prom_state(uint32_t config)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_PROM_PP1FILLMODE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xffffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_CONFIG, config));
}

static void mgras_prom_block(uint32_t fillmode, uint32_t x0, uint32_t y0,
                             uint32_t x1, uint32_t y1)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, fillmode));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (x0 << 16) | y0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (x1 << 16) | y1));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_PROM_BLOCK, 0x18));
}

/*
 * The PROM paints its backdrop a row at a time: RED's index in a block of
 * FILLMODE 0x00018000, started by a register 0x013 execute. Row 5 counted
 * from the bottom is screen line 1018. Under the op bits alone the paint
 * would CLEAR, and under bit 13 alone it would store RGB from PACKEDCOLOR;
 * either leaves the pixel 0.
 */
static void test_prom_ink_fill_in_hardware_rows(void)
{
    const uint32_t row = MGRAS_SCREEN_HEIGHT - 1 - 5;

    mgras_reset();
    mgras_prom_state(MGRAS_PROM_CONFIG);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RED, 0x60u << 12));
    mgras_prom_block(MGRAS_FILLMODE_PROM_INK_FILL, 10, 5, 20, 5);

    g_assert_cmphex(mgras_test_pixel(qts, 10, row), ==, 0x60);
    g_assert_cmphex(mgras_test_pixel(qts, 20, row), ==, 0x60);
    g_assert_cmphex(mgras_test_format(qts, 15, row), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_test_pixel(qts, 9, row), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 21, row), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 15, row + 1), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 15, 5), ==, 0);
}

/*
 * A PROM glyph: FILLMODE 0x00418008 and a register 0x013 execute start the
 * block, then each CHAR_H execute paints the next row up from the block's
 * bottom row in RED's index, leaving clear bits alone.
 */
static void test_prom_glyph_rows_in_hardware_rows(void)
{
    const uint32_t bottom = MGRAS_SCREEN_HEIGHT - 1 - 100;

    mgras_reset();
    mgras_prom_state(MGRAS_PROM_CONFIG);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RED, 0x30u << 12));
    mgras_prom_block(MGRAS_FILLMODE_PROM_GLYPH, 40, 100, 47, 101);
    g_assert_cmphex(mgras_test_pixel(qts, 40, bottom), ==, 0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x81u << 24));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x18u << 24));

    g_assert_cmphex(mgras_test_pixel(qts, 40, bottom), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 41, bottom), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 47, bottom), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 40, bottom - 1), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 43, bottom - 1), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 44, bottom - 1), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 40, 100), ==, 0);
}

/*
 * A PROM glyph wider than 32 columns: CHAR_H is only written, and the
 * CHAR_L execute paints the row from both, CHAR_H bits 31:0 as columns
 * 0-31 and CHAR_L bits 31:0 as columns 32-63, stopping at the block's
 * right edge. The PROM draws its menu icons this way (blocks 34 to 38
 * wide, one CHAR_L execute per row).
 */
static void test_prom_wide_glyph_row_on_char_l(void)
{
    const uint32_t bottom = MGRAS_SCREEN_HEIGHT - 1 - 100;

    mgras_reset();
    mgras_prom_state(MGRAS_PROM_CONFIG);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RED, 0x30u << 12));
    mgras_prom_block(MGRAS_FILLMODE_PROM_GLYPH, 40, 100, 77, 101);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x80000001u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0xc6000000u));

    g_assert_cmphex(mgras_test_pixel(qts, 40, bottom), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 41, bottom), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 71, bottom), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 72, bottom), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 73, bottom), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 74, bottom), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 76, bottom), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 77, bottom), ==, 0x30);
    /* CHAR_L bit 25 would be column 78, past the block, and is dropped. */
    g_assert_cmphex(mgras_test_pixel(qts, 78, bottom), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 40, bottom - 1), ==, 0);
}

/* With CONFIG bit 3 set the same glyph's rows run top-down from row 100. */
static void test_prom_glyph_rows_top_down_with_config_bit_3(void)
{
    mgras_reset();
    mgras_prom_state(MGRAS_PROM_CONFIG | MGRAS_CONFIG_TOP_DOWN);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RED, 0x30u << 12));
    mgras_prom_block(MGRAS_FILLMODE_PROM_GLYPH, 40, 100, 47, 101);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x81u << 24));

    g_assert_cmphex(mgras_test_pixel(qts, 40, 100), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 40, MGRAS_SCREEN_HEIGHT - 1 - 100),
                    ==, 0);
}

/*
 * A bottom-up block that runs past the top of the screen clips there, as
 * a top-down fill clips at the bottom: rows 1020-1030 counted from the
 * bottom paint screen lines 3 to 0.
 */
static void test_prom_ink_fill_clips_past_the_top(void)
{
    mgras_reset();
    mgras_prom_state(MGRAS_PROM_CONFIG);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RED, 0x60u << 12));
    mgras_prom_block(MGRAS_FILLMODE_PROM_INK_FILL, 10, 1020, 20, 1030);

    g_assert_cmphex(mgras_test_pixel(qts, 10, 0), ==, 0x60);
    g_assert_cmphex(mgras_test_pixel(qts, 20, 3), ==, 0x60);
    g_assert_cmphex(mgras_test_pixel(qts, 15, 4), ==, 0);
}

/* A register 0x013 execute under any other FILLMODE paints nothing. */
static void test_prom_block_other_fillmode_is_inert(void)
{
    mgras_reset();
    mgras_prom_state(MGRAS_PROM_CONFIG);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RED, 0x60u << 12));
    mgras_prom_block(0x00100000, 10, 5, 20, 5);

    g_assert_cmphex(mgras_test_pixel(qts, 10, MGRAS_SCREEN_HEIGHT - 1 - 5),
                    ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 10, 5), ==, 0);
}

/*
 * A row up to 32 columns wide executes entirely on CHAR_H; CHAR_L is never
 * written. Markers sit at columns 0, 15, 16 and 19 to exercise both CHAR_H
 * halfwords and the row's last column. A second CHAR_H execute paints a
 * different pattern on the next row: if a single execute painted a pair of
 * rows (the row-pair theory this replaces), the first execute would already
 * have reached row 1, and the second execute's pattern would land on row 2
 * instead of row 1.
 */
static void test_direct_glyph_row_char_h_only(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* A glyph row reads PACKEDCOLOR only once PP1FILLMODE selects RGB. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00ffffff)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (80u << 16) | 80u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (99u << 16) | 81u));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x80019000u));

    g_assert_cmphex(mgras_test_pixel(qts, 80, 80), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 81, 80), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 94, 80), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 95, 80), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 96, 80), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 97, 80), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 98, 80), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 99, 80), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 80), ==, 0);

    /* The single execute above must not have reached row 81 yet. */
    g_assert_cmphex(mgras_test_pixel(qts, 80, 81), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 85, 81), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 90, 81), ==, 0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x04200000u));

    g_assert_cmphex(mgras_test_pixel(qts, 80, 81), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 85, 81), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 86, 81), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 90, 81), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 91, 81), ==, 0);
    /* Row 1's pattern differs from row 0's; no bleed from the first row. */
    g_assert_cmphex(mgras_test_pixel(qts, 95, 81), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 99, 81), ==, 0);
}


/*
 * A 64-column row executes entirely on CHAR_L (CHAR_H is staged first and
 * never itself executes). Markers sit in all four halfwords -- CHAR_H's
 * upper and lower half, CHAR_L's upper and lower half -- including both
 * ends of the CHAR_H/CHAR_L boundary at columns 31 and 32, to pin down the
 * bit order across the combined 64-bit row.
 */
static void test_direct_glyph_row_char_l_execute(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* A glyph row reads PACKEDCOLOR only once PP1FILLMODE selects RGB. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00ffffff)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (263u << 16) | 200u));

    /*
     * Column 3: CHAR_H's upper halfword. Column 31: CHAR_H's lower
     * halfword, the last column CHAR_H sources.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x10000001u));
    /*
     * Column 32: CHAR_L's upper halfword, the first column CHAR_L sources.
     * Column 61: CHAR_L's lower halfword.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x80000004u));

    g_assert_cmphex(mgras_test_pixel(qts, 200, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 202, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 203, 200), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 204, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 230, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 231, 200), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 232, 200), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 233, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 260, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 261, 200), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 262, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 263, 200), ==, 0);

    /* The 64-column row is exactly this rect's width; nothing past it. */
    g_assert_cmphex(mgras_test_pixel(qts, 264, 200), ==, 0);
}


/*
 * A glyph row's CI8 ink is RED>>12&0xff (contract rule 2), not FILLCOLOR_R:
 * the reset default format is already CI8, so no PP1FILLMODE write is
 * needed here. A clear bit paints nothing at all, distinct from painting a
 * zero background: pre-paint the destination row to a nonzero value with a
 * plain rect fill, then confirm a glyph row's clear-bit columns still read
 * that pre-painted value afterward while its set-bit columns take the ink.
 */
static void test_direct_glyph_row_ci8_reads_red_channel(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x22));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (47u << 16) | 47u));
    g_assert_cmphex(mgras_test_pixel(qts, 40, 40), ==, 0x22);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_RED, 0x9a000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (47u << 16) | 47u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x30000000u));

    /* Set-bit columns (2 and 3) take the RED>>12 ink, 0x9a. */
    g_assert_cmphex(mgras_test_pixel(qts, 42, 40), ==, 0x9a);
    g_assert_cmphex(mgras_test_pixel(qts, 43, 40), ==, 0x9a);

    /* Clear-bit columns keep the fill's 0x22 untouched, not a bg colour. */
    g_assert_cmphex(mgras_test_pixel(qts, 40, 40), ==, 0x22);
    g_assert_cmphex(mgras_test_pixel(qts, 41, 40), ==, 0x22);
    g_assert_cmphex(mgras_test_pixel(qts, 44, 40), ==, 0x22);
    g_assert_cmphex(mgras_test_pixel(qts, 47, 40), ==, 0x22);
}


/*
 * FILLMODE_BYTE_XFER delivers eight indexed pixels per CHAR_L execute when
 * register 0x159 selects the packing (see REG_BYTE_XFER_PACKING); this is
 * also the check that 0x159 == 0x8000 keeps that eight-pixel path. An
 * 18-column-wide rect makes the row's skip into the 8-byte unit alternate
 * 0, 4, 0, ... per row (skip = (row * pitch) % 8, the pitch 18 plus the
 * padding field's 2 in 0x159 bits 16:14), covering both values in two
 * rows. Each unit writes a fresh CHAR_H/CHAR_L pair with
 * distinct byte markers so a painted pixel's value names the byte that
 * produced it.
 */
static void test_direct_byte_row(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* The transfer paints colour only; the WID comes from a plane fill. */
    mgras_tag_wid(qts, 120, 120, 137, 121, 5);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BYTE_XFER_PACKING,
                            MGRAS_BYTE_XFER_PACKING_BYTES));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 5u << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (120u << 16) | 120u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (137u << 16) | 121u));

    /* Row 0, skip 0: three 8-byte units cover columns 0-17. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x00010203u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x04050607u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x08090a0bu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x0c0d0e0fu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x10111213u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x14151617u));

    g_assert_cmphex(mgras_test_pixel(qts, 120, 120), ==, 0x00);
    g_assert_cmphex(mgras_test_pixel(qts, 127, 120), ==, 0x07);
    g_assert_cmphex(mgras_test_pixel(qts, 128, 120), ==, 0x08);
    g_assert_cmphex(mgras_test_pixel(qts, 137, 120), ==, 0x11);
    g_assert_cmphex(mgras_test_wid(qts, 120, 120), ==, 5);

    /* Row 1, skip 4: the first unit's low nibble lands on columns 0-3. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x28292a2bu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x2c2d2e2fu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x30313233u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x34353637u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x38393a3bu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x3c3d3e3fu));

    g_assert_cmphex(mgras_test_pixel(qts, 120, 121), ==, 0x2c);
    g_assert_cmphex(mgras_test_pixel(qts, 123, 121), ==, 0x2f);
    g_assert_cmphex(mgras_test_pixel(qts, 124, 121), ==, 0x30);
    g_assert_cmphex(mgras_test_pixel(qts, 137, 121), ==, 0x3d);
    g_assert_cmphex(mgras_test_wid(qts, 137, 121), ==, 5);
}

/*
 * The eight-pixel transfer's source pitch is the width plus 0x159 bits
 * 16:14, not the width rounded up to 32 bits. The IRIX 6.5.30 greeter
 * restores the 6-pixel-wide area under its text cursor from a buffer of
 * pitch 12 (the pitch of the DMA read that saved it) with 0x159 ==
 * 0x18000 (field 6): row 1 starts 4 bytes into the
 * second unit and needs two units, row 2 starts a fresh unit. A 32-bit
 * pitch of 8 would start every row at a unit boundary and paint row 1
 * from the padding bytes 0x08-0x0b.
 */
static void test_direct_byte_row_pitch_from_padding_field(void)
{
    static const uint32_t units[4][2] = {
        { 0x00010203u, 0x04050607u },
        { 0x08090a0bu, 0x0c0d0e0fu },
        { 0x10111213u, 0x14151617u },
        { 0x18191a1bu, 0x1c1d1e1fu },
    };

    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BYTE_XFER_PACKING, 0x18000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (454u << 16) | 700u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (459u << 16) | 702u));
    for (int i = 0; i < 4; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_CHAR_H, units[i][0]));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_CHAR_L, units[i][1]));
    }

    /* Row 0: source bytes 0-5. */
    g_assert_cmphex(mgras_test_pixel(qts, 454, 700), ==, 0x00);
    g_assert_cmphex(mgras_test_pixel(qts, 459, 700), ==, 0x05);
    /* Row 1: source bytes 12-17, across the second and third units. */
    g_assert_cmphex(mgras_test_pixel(qts, 454, 701), ==, 0x0c);
    g_assert_cmphex(mgras_test_pixel(qts, 457, 701), ==, 0x0f);
    g_assert_cmphex(mgras_test_pixel(qts, 458, 701), ==, 0x10);
    g_assert_cmphex(mgras_test_pixel(qts, 459, 701), ==, 0x11);
    /* Row 2: source bytes 24-29, the fourth unit. */
    g_assert_cmphex(mgras_test_pixel(qts, 454, 702), ==, 0x18);
    g_assert_cmphex(mgras_test_pixel(qts, 459, 702), ==, 0x1d);
}


/*
 * Register 0x159 == 0x0080 (the chooser icons' packing, see
 * tests/data/mgras/root-icon.replay) delivers two indexed pixels per
 * CHAR_L execute: pixel 0 from CHAR_H bits 7:0, pixel 1 from CHAR_L bits
 * 7:0. A 6-wide, 3-row rect needs ceil(6 / 2) = 3 executes per row, no
 * skip. Every execute's other three bytes are a constant, non-replicated
 * marker distinct from the low byte, to pin down that only bits 7:0 feed
 * the pixel.
 */
static void test_direct_byte_row_pairs(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    mgras_enter_context(qts);

    mgras_tag_wid(qts, 200, 200, 205, 202, 9);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BYTE_XFER_PACKING,
                            MGRAS_BYTE_XFER_PACKING_PAIRS));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (205u << 16) | 202u));

    for (int row = 0; row < 3; row++) {
        for (int unit = 0; unit < 3; unit++) {
            uint8_t pixel0 = 0x10 + row * 0x10 + unit * 2;
            uint8_t pixel1 = pixel0 + 1;

            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                         MGRAS_COMMAND(REG_CHAR_H, 0x12345600u | pixel0));
            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                         MGRAS_COMMAND_EXEC(REG_CHAR_L, 0xabcdef00u | pixel1));
        }
    }

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 6; col++) {
            g_assert_cmphex(mgras_test_pixel(qts, 200 + col, 200 + row), ==,
                            0x10 + row * 0x10 + col);
        }
    }
    g_assert_cmphex(mgras_test_wid(qts, 200, 200), ==, 9);
    g_assert_cmphex(mgras_test_wid(qts, 205, 202), ==, 9);

    qtest_quit(qts);
}


/*
 * The real xdm chooser ORs the row-padding field (bits 16:14) into the
 * same 0x159 word as the packing selector bit, e.g. 0x00010080 rather
 * than the bare 0x0080 the test above drives: row padding to 8 bytes is
 * in bits 16:14 and caller flags are in bits 7:4 and 3:0.
 * mgras_draw_byte_row()'s packing switch used an exact
 * match against the whole register value, so every one of these calls
 * fell to the LOG_UNIMP default and was dropped -- 252 drops in a single
 * capture, wiping icon glyph and button/field content
 * (dropped_byte_row_geometry.json). The switch must read the selector
 * bits only.
 */
static void test_direct_byte_row_pairs_row_pad_bits(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    mgras_enter_context(qts);

    mgras_tag_wid(qts, 500, 200, 505, 200, 9);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BYTE_XFER_PACKING,
                            MGRAS_BYTE_XFER_PACKING_PAIRS | 0x00010000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (500u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (505u << 16) | 200u));

    for (int unit = 0; unit < 3; unit++) {
        uint8_t pixel0 = 0x10 + unit * 2;
        uint8_t pixel1 = pixel0 + 1;

        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_CHAR_H, 0x12345600u | pixel0));
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_CHAR_L, 0xabcdef00u | pixel1));
    }

    for (int col = 0; col < 6; col++) {
        g_assert_cmphex(mgras_test_pixel(qts, 500 + col, 200), ==, 0x10 + col);
    }
    g_assert_cmphex(mgras_test_wid(qts, 500, 200), ==, 9);

    qtest_quit(qts);
}


/*
 * Same masking bug, the eight-pixel path: register 0x159 = 0x00018000,
 * observed 120x in the same capture over rectangle (454,700)-(459,719)
 * (a button/field-row glyph).
 */
static void test_direct_byte_row_with_row_pad_bits(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    mgras_tag_wid(qts, 500, 210, 507, 210, 5);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BYTE_XFER_PACKING,
                            MGRAS_BYTE_XFER_PACKING_BYTES | 0x00010000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 5u << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (500u << 16) | 210u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (507u << 16) | 210u));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x00010203u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x04050607u));

    g_assert_cmphex(mgras_test_pixel(qts, 500, 210), ==, 0x00);
    g_assert_cmphex(mgras_test_pixel(qts, 503, 210), ==, 0x03);
    g_assert_cmphex(mgras_test_pixel(qts, 507, 210), ==, 0x07);
    g_assert_cmphex(mgras_test_wid(qts, 500, 210), ==, 5);
}


/*
 * The desktop backdrop's 4x4 seed weave: register 0x159 == 0 (neither bit
 * 15 nor bit 7 set), a value mgras_draw_byte_row() used to route to the
 * unimplemented default and drop. Captured trace values:
 * box (0,0)-(3,3), then four STATE CHAR_H / EXEC CHAR_L pairs painting the
 * X root window's weave (one set pixel per row, colour index 7 on 0):
 *
 *   0 0 0 7
 *   0 7 0 0
 *   0 0 7 0
 *   7 0 0 0
 *
 * with the existing eight-byte path's skip = (row * 4 * ceil(width/4)) % 8
 * rule alternating which half of each CHAR_H/CHAR_L pair lands on which
 * row, exactly as the real capture painted it.
 */
static void test_direct_byte_row_seed_packing(void)
{
    static const uint8_t weave[4][4] = {
        { 0, 0, 0, 7 },
        { 0, 7, 0, 0 },
        { 0, 0, 7, 0 },
        { 7, 0, 0, 0 },
    };

    mgras_reset();

    mgras_enter_context(qts);

    mgras_tag_wid(qts, 0, 0, 3, 3, 5);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BYTE_XFER_PACKING, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (3u << 16) | 3u));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x00000007u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x00070000u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x00000007u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x00070000u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x00000700u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x07000000u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x00000700u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x07000000u));

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            g_assert_cmphex(mgras_test_pixel(qts, col, row), ==,
                            weave[row][col]);
        }
    }
    g_assert_cmphex(mgras_test_wid(qts, 0, 0), ==, 5);
}


/*
 * There is no direct-colour path: a fill's low byte indexes the map its
 * WID's XMAP mode selects, and the conversion to the host surface puts
 * entry bits 31:24, 23:16 and 15:8 out as red, green and blue.
 */
static void test_display_surface_conversion(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* WID 0 resolves through the reset mode 0x7a4, which selects map 29. */
    mgras_write_cmap_entry(qts, 29, 0x33, 0x33221100);
    mgras_write_cmap_entry(qts, 29, 0xcc, 0xccbbaa00);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x33));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));
    g_assert_cmphex(mgras_test_pixel(qts, 11, 11), ==, 0x33);
    g_assert_cmphex(mgras_test_display_pixel(qts, 11, 11), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    g_assert_cmphex(mgras_test_display_pixel(qts, 11, 11), ==, 0x00332211);

    g_assert_cmphex(mgras_test_display_pixel(qts, 20, 11), ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xcc));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (31u << 16) | 31u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 30, 30), ==, 0x00ccbbaa);

    g_assert_cmphex(mgras_test_display_pixel(qts, 11, 11), ==, 0x00332211);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x0));
    g_assert_cmphex(mgras_test_display_pixel(qts, 11, 11), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 30, 30), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 30, 30), ==, 0xcc);
}

static void test_reset_refreshes_display_surface(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 0x33, 0x33221100);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x33));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (14u << 16) | 14u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (14u << 16) | 14u));
    g_assert_cmphex(mgras_screendump_pixel(qts, 14, 14), ==, 0x00332211);
    g_assert_cmphex(mgras_test_display_pixel(qts, 14, 14), ==, 0x00332211);

    qtest_system_reset(qts);
    g_assert_cmphex(mgras_screendump_pixel(qts, 14, 14), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 14, 14), ==, 0);
}


static void test_pseudocolor_scanout_looks_up_cmap(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    mgras_write_cmap_position(qts, 0x001d, 17, 0xecec0000);
    mgras_write_cmap_entry(qts, 29, 0x33, 0x44556600);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x11));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_pixel(qts, 5, 5), ==, 0x11);
    g_assert_cmphex(mgras_screendump_pixel(qts, 5, 5), ==, 0x00ecec00);
    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00ecec00);

    mgras_write_cmap_position(qts, 0x001d, 17, 0x11223300);
    g_assert_cmphex(mgras_screendump_pixel(qts, 5, 5), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00112233);


    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x7a4);
    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00112233);


    /* A rect fill indexes too: 0x33 is an index, not a blue byte. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x33));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (6u << 16) | 6u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (6u << 16) | 6u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 6, 6), ==, 0x00445566);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00445566);
}

/*
 * The guest packs CMAP entries left-justified (0xRRGGBB00); mgras_cmap_rgb
 * must read bits 31:8, not bits 23:0.
 */
static void test_cmap_high_byte_packing(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeb(qts, REG_XMAP_INDEX, 3 << 2);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 17 << 5);

    mgras_write_cmap_position(qts, 0x0011, 17, 0xffffff00);

    mgras_tag_wid(qts, 7, 7, 7, 7, 3);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x11));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (7u << 16) | 7u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (7u << 16) | 7u));
    g_assert_cmphex(mgras_test_pixel(qts, 7, 7), ==, 0x11);
    g_assert_cmphex(mgras_test_wid(qts, 7, 7), ==, 3);
    g_assert_cmphex(mgras_test_display_pixel(qts, 7, 7), ==, 0x00ffffff);
}

/*
 * A CI8 pixel's format tag routes scanout through the WID's colour map; an
 * RGB pixel's tag bypasses it entirely (contract rule 6's gamma still
 * applies to both). Paint one of each at different locations under one
 * WID and confirm each keeps its own tag and resolves independently: the
 * RGB pixel's FILLCOLOR bytes do not coincide with the CI8 index or its
 * mapped entry, so a display pixel that matched the CMAP entry instead of
 * the direct colour would show the map leaked into the RGB path.
 */
static void test_pixel_format_tag_resolves_independently(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    /* WID 0 resolves through the reset mode 0x7a4, which selects map 29. */
    mgras_write_cmap_entry(qts, 29, 0x40, 0x99887700);

    /* A CI8 fill: reset default format, FILLCOLOR_R read as a bare index. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (10u << 16) | 10u));
    g_assert_cmphex(mgras_test_pixel(qts, 10, 10), ==, 0x40);
    g_assert_cmphex(mgras_test_format(qts, 10, 10), ==, MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_test_display_pixel(qts, 10, 10), ==, 0x00998877);

    /* An RGB fill elsewhere: FILLCOLOR_R/G/B each >> 4, no CMAP lookup. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) | MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_R, 0x100));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_G, 0x200));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLCOLOR_B, 0x300));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (20u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (20u << 16) | 20u));
    g_assert_cmphex(mgras_test_pixel(qts, 20, 20), ==, 0x00102030);
    g_assert_cmphex(mgras_test_format(qts, 20, 20), ==, MGRAS_PIXEL_FORMAT_RGB);
    g_assert_cmphex(mgras_test_display_pixel(qts, 20, 20), ==, 0x00102030);

    /* Switching the live format register does not retag the CI8 pixel. */
    g_assert_cmphex(mgras_test_format(qts, 10, 10), ==, MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_test_display_pixel(qts, 10, 10), ==, 0x00998877);
}

static void mgras_write_fill_color(QTestState *qts, uint32_t r, uint32_t g,
                                   uint32_t b)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, r));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_G, g));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_B, b));
}

/*
 * A 12-bit RGB window's fill keeps the top four bits of each channel in
 * the pixel's low 12 bits, red lowest as the server's 12-bit TrueColor
 * visual masks say, and scanout widens each back to eight bits by
 * repeating it. The values are the IRIX 6.5.30 Icon
 * Catalog's: its pane is cleared under 0xc804 with every plane open,
 * then redrawn under 0x4004 with COLORMASKLSBSA 0xfff, and FILLCOLOR
 * 0x500/0x800/0xa00 is the pane's blue.
 */
static void test_rgb12_fill_packs_four_bit_channels(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) |
                               MGRAS_PP1FILLMODE_RGB12_CLEAR));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    mgras_write_fill_color(qts, 0x500, 0x800, 0xa00);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));
    g_assert_cmphex(mgras_test_pixel(qts, 10, 10), ==, 0xa85);
    g_assert_cmphex(mgras_test_format(qts, 10, 10), ==,
                    MGRAS_PIXEL_FORMAT_RGB12);
    g_assert_cmphex(mgras_test_display_pixel(qts, 10, 10), ==, 0x005588aa);

    /* Drawing under 0x4004 through the 12-bit mask: a grey, then white. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) |
                               MGRAS_PP1FILLMODE_RGB12));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xfff));
    mgras_write_fill_color(qts, 0x5ff, 0x500, 0x50f);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (11u << 16) | 11u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (11u << 16) | 11u));
    g_assert_cmphex(mgras_test_pixel(qts, 11, 11), ==, 0x555);
    g_assert_cmphex(mgras_test_display_pixel(qts, 11, 11), ==, 0x00555555);
    mgras_write_fill_color(qts, 0xf00, 0xf00, 0xf00);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (12u << 16) | 12u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (12u << 16) | 12u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 12, 12), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 13, 13), ==, 0x005588aa);

    /* The desktop's CI8 encoding, 0x4504, still paints an index. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) | 0x4504));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x1b));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (20u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (20u << 16) | 20u));
    g_assert_cmphex(mgras_test_pixel(qts, 20, 20), ==, 0x1b);
    g_assert_cmphex(mgras_test_format(qts, 20, 20), ==, MGRAS_PIXEL_FORMAT_CI8);
}

/*
 * Lines and glyph rows in a 12-bit window take the same four-bit channels
 * from their whole-pixel sources: PACKEDCOLOR for set bits, BKGRD for an
 * opaque stipple's clear bits. The Icon Catalog draws its icons' bodies
 * as opaque stippled spans of PACKEDCOLOR 0xa0a0a0 on white and its
 * labels as glyph rows of PACKEDCOLOR 0. The test's PACKEDCOLOR values
 * give red and blue the same byte, so they hold whichever end of the
 * word red is read from.
 */
static void test_rgb12_line_and_glyph_take_top_nibbles(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) |
                               MGRAS_PP1FILLMODE_RGB12));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xfff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_STIPPLE_OPAQUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x30a030));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_RG, 0xf00f00));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_BA, 0xf00));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_LINE_STIPPLE, 0xaaaaaaaa));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (30u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (33u << 16) | 30u));
    g_assert_cmphex(mgras_test_pixel(qts, 30, 30), ==, 0x3a3);
    g_assert_cmphex(mgras_test_format(qts, 30, 30), ==,
                    MGRAS_PIXEL_FORMAT_RGB12);
    g_assert_cmphex(mgras_test_display_pixel(qts, 30, 30), ==, 0x0033aa33);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 30), ==, 0xfff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 31, 30), ==, 0x00ffffff);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x10201f));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (47u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x80000000));
    g_assert_cmphex(mgras_test_pixel(qts, 40, 40), ==, 0x121);
    g_assert_cmphex(mgras_test_display_pixel(qts, 40, 40), ==, 0x00112211);
    g_assert_cmphex(mgras_test_pixel(qts, 41, 40), ==, 0);
}

/*
 * A whole-pixel word keeps red in bits 7:0 in a 12-bit window too. The
 * Icon Catalog's soffice icon is stippled with PACKEDCOLOR 0xf00000,
 * which the RAD4 reference shows blue; read red-first it showed red.
 */
static void test_rgb12_pixel_word_reads_red_low(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) |
                               MGRAS_PP1FILLMODE_RGB12));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xfff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0xf00000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (47u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_H, 0x80000000));
    g_assert_cmphex(mgras_test_pixel(qts, 40, 40), ==, 0xf00);
    g_assert_cmphex(mgras_test_display_pixel(qts, 40, 40), ==, 0x000000ff);
}

/*
 * Two tagged pixels, two XMAP modes, two colour maps, one shared index.
 * Each resolves through its own map, and a palette write to one map
 * leaves the other pixel alone.
 */
static void test_target_first_cmap_visual_selection(void)
{
    uint32_t native_a, native_b, wid_a, wid_b;

    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeb(qts, REG_XMAP_INDEX, 0x0c);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 15 << 5);
    qtest_writeb(qts, REG_XMAP_INDEX, 0x24);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 29 << 5);

    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0xe00f);
    for (int position = 0; position < 16; position++) {
        qtest_writel(qts, REG_CMAP_DATA, 0x00010101 + position * 0x00010101);
    }
    qtest_writel(qts, REG_CMAP_DATA, 0x11223300);
    for (int position = 17; position < 32; position++) {
        qtest_writel(qts, REG_CMAP_DATA, 0x00202020 + position * 0x00010101);
    }
    qtest_writel(qts, REG_CMAP_DATA, 0x00040506);

    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0xe01d);
    for (int position = 0; position < 16; position++) {
        qtest_writel(qts, REG_CMAP_DATA, 0x00404040 + position * 0x00010101);
    }
    qtest_writel(qts, REG_CMAP_DATA, 0xa1b2c300);

    mgras_tag_wid(qts, 20, 20, 20, 20, 3);
    mgras_tag_wid(qts, 21, 20, 21, 20, 9);

    /* The paint carries a third DRBPOINTERS value and must not re-tag. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 0x1f << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xf0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (20u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (21u << 16) | 20u));

    native_a = mgras_test_pixel(qts, 20, 20);
    native_b = mgras_test_pixel(qts, 21, 20);
    wid_a = mgras_test_wid(qts, 20, 20);
    wid_b = mgras_test_wid(qts, 21, 20);
    g_assert_cmphex(native_a, ==, 0xf0);
    g_assert_cmphex(native_b, ==, 0xf0);
    g_assert_cmphex(wid_a, ==, 3);
    g_assert_cmphex(wid_b, ==, 9);
    g_assert_cmphex(mgras_test_display_pixel(qts, 20, 20), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(qts, 21, 20), ==, 0x00a1b2c3);

    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0xe00f);
    for (int position = 0; position < 16; position++) {
        qtest_writel(qts, REG_CMAP_DATA, 0);
    }
    qtest_writel(qts, REG_CMAP_DATA, 0x01020300);
    g_assert_cmphex(mgras_test_pixel(qts, 20, 20), ==, native_a);
    g_assert_cmphex(mgras_test_pixel(qts, 21, 20), ==, native_b);
    g_assert_cmphex(mgras_test_wid(qts, 20, 20), ==, wid_a);
    g_assert_cmphex(mgras_test_wid(qts, 21, 20), ==, wid_b);
    g_assert_cmphex(mgras_test_display_pixel(qts, 20, 20), ==, 0x00010203);
    g_assert_cmphex(mgras_test_display_pixel(qts, 21, 20), ==, 0x00a1b2c3);
}


/*
 * A paint that writes nothing -- a zero colour mask, or a logic operation
 * that reproduces the destination -- leaves the native colour, the WID and
 * therefore the displayed pixel exactly as they were.
 */
static void test_masked_out_paint_keeps_the_visual(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_position(qts, 0x001d, 17, 0xecec0000);
    mgras_write_cmap_entry(qts, 29, 0x33, 0x33221100);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x11));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (512u << 16) | 512u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (767u << 16) | 767u));
    g_assert_cmphex(mgras_test_pixel(qts, 640, 512), ==, 0x11);
    g_assert_cmphex(mgras_test_display_pixel(qts, 640, 512), ==, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKMSBS, 0xf0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, 0x0013f500));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 194u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (1279u << 16) | 1023u));
    g_assert_cmphex(mgras_test_pixel(qts, 640, 512), ==, 0x11);
    g_assert_cmphex(mgras_test_display_pixel(qts, 640, 512), ==, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (1279u << 16) | 1023u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 640, 512), ==, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_XOR << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKMSBS, 0x70));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (630u << 16) | 512u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (650u << 16) | 512u));
    g_assert_cmphex(mgras_test_pixel(qts, 640, 512), ==, 0x11);
    g_assert_cmphex(mgras_test_display_pixel(qts, 640, 512), ==, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x33));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (640u << 16) | 512u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (640u << 16) | 512u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 640, 512), ==, 0x00332211);
}


/*
 * A window move repaints by retagging or partially recolouring pixels
 * that were already on screen, not just by filling a fresh rectangle.
 * Each of the three paints below must still leave the QMP screendump
 * agreeing with the model's own display-pixel readback.
 */
static void test_paint_variants_refresh_the_surface(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* WID 2 resolves through map 5, WID 3 through map 6. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 2));
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 5 << 5);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 3));
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 6 << 5);
    mgras_write_cmap_entry(qts, 5, 0x22, 0x11223300);
    mgras_write_cmap_entry(qts, 6, 0x22, 0x44556600);

    /* Baseline: a colour fill tagged WID 2, index 0x22 through map 5. */
    mgras_tag_wid(qts, 200, 200, 207, 207, 2);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x22));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (207u << 16) | 207u));
    g_assert_cmphex(mgras_screendump_pixel(qts, 203, 203), ==,
                    mgras_test_display_pixel(qts, 203, 203));

    /*
     * (a) A WID-only fill retags the same rectangle to WID 3 without
     * touching its colour byte, moving it to map 6.
     */
    mgras_tag_wid(qts, 200, 200, 207, 207, 3);
    g_assert_cmphex(mgras_test_wid(qts, 203, 203), ==, 3);
    g_assert_cmphex(mgras_test_pixel(qts, 203, 203), ==, 0x22);
    g_assert_cmphex(mgras_screendump_pixel(qts, 203, 203), ==,
                    mgras_test_display_pixel(qts, 203, 203));

    /* (b) A rect fill masked to the colour's low byte only. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x66));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (207u << 16) | 207u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    g_assert_cmphex(mgras_screendump_pixel(qts, 203, 203), ==,
                    mgras_test_display_pixel(qts, 203, 203));

    /* (c) A FILLMODE byte-transfer row. */
    mgras_tag_wid(qts, 220, 220, 227, 221, 2);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (220u << 16) | 220u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (227u << 16) | 221u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CHAR_H, 0x22222222u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_CHAR_L, 0x22222222u));
    g_assert_cmphex(mgras_screendump_pixel(qts, 223, 220), ==,
                    mgras_test_display_pixel(qts, 223, 220));
}

static void test_cmap_selector_addresses_the_right_slot(void)
{
    mgras_reset();

    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x2005);
    for (int position = 0; position < 32; position++) {
        qtest_writel(qts, REG_CMAP_DATA, 0x00100000 + position);
    }
    qtest_writel(qts, REG_CMAP_DATA, 0x00aabbcc);

    /* Selector 0x2005 starts at entry 32, so the 33rd write is entry 64. */
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x2005);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0x00100000);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (64 << 8) | 5);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0x00aabbcc);

    /* A run wraps at the 256-entry map boundary, inside the same map. */
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0xff05);
    qtest_writel(qts, REG_CMAP_DATA, 0x00ddeeff);
    qtest_writel(qts, REG_CMAP_DATA, 0x00123456);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0xff05);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0x00ddeeff);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x0005);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0x00123456);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0xff04);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0);
}

/*
 * The X server allocates single colour cells one selector at a time. Those
 * selectors differ only in bits 15:8, so a decode that dropped that field
 * would stack all of them onto one entry.
 */
static void test_cmap_selector_start_entry(void)
{
    mgras_reset();

    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x1011);
    qtest_writel(qts, REG_CMAP_DATA, 0xc1c1c100);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x2c11);
    qtest_writel(qts, REG_CMAP_DATA, 0x8b6a6a00);

    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (16 << 8) | 17);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0xc1c1c100);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (44 << 8) | 17);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0x8b6a6a00);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x0011);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (32 << 8) | 17);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0);

    /* A run advances one entry per write from the selected entry. */
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x1011);
    qtest_writel(qts, REG_CMAP_DATA, 0x11111100);
    qtest_writel(qts, REG_CMAP_DATA, 0x22222200);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (17 << 8) | 17);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0x22222200);

    /* The same entry in the neighbouring map is untouched. */
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (16 << 8) | 18);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0);
}


static void test_fifo_lowater_interrupt(void)
{
    const unsigned int lowater_vector = 60;
    const unsigned int error_vector = 61;
    const uint64_t expected_addr =
        ((uint64_t)HQ_WIDGET << 48) | HQ_REG_OFFSET;

    mgras_reset();

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS, lowater_vector);


    qtest_writel(qts, REG_IRQ_MASK, 0);


    qtest_writel(qts, REG_HQ_ENABLE, MGRAS_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_LOWATER, ==,
                     MGRAS_PENDING_LOWATER);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector | 0x100);


    qtest_writel(qts, REG_HQ_ENABLE_CLEAR, MGRAS_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE) & MGRAS_HQ_ENABLE_LOWATER,
                     ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_LOWATER, ==,
                    0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector);


    qtest_writel(qts, REG_HQ_ENABLE, MGRAS_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_LOWATER, ==,
                     MGRAS_PENDING_LOWATER);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector | 0x100);


    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_LOWATER, ==,
                    0);
    qtest_writel(qts, REG_HQ_ENABLE, MGRAS_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_LOWATER, ==,
                    0);
}


static void test_sched_arg_4_round_trip(void)
{
    mgras_reset();

    qtest_writel(qts, REG_SCHED_ARG_4, 0x1885);
    g_assert_cmphex(qtest_readl(qts, REG_SCHED_ARG_4), ==, 0x1885);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_fifo_threshold_registers(void)
{
    mgras_reset();

    qtest_writel(qts, REG_FIFO_THRESHOLD_0, 0x47);
    qtest_writel(qts, REG_FIFO_THRESHOLD_1, 0x14);
    qtest_writel(qts, REG_FIFO_THRESHOLD_LAST, 0);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_0), ==, 0x47);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_1), ==, 0x14);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_LAST), ==, 0);
}


static void test_hq_flags_set_and_clear(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0);


    qtest_writel(qts, REG_HQ_FLAGS, 0x100000);
    qtest_writel(qts, REG_HQ_FLAGS, 0x000010);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0x100010);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS_CLEAR), ==, 0x100010);


    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, 0x100000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0x000010);
}

/*
 * Two more queries wait on flag bit 17 and read 0x028004:
 * 0xa104 with one index, which the current-attribute query routine sends for
 * GL_CURRENT_COLOR (indices 4 to 7, read as floats) and the other
 * current attributes, and 0x8b08 with two words, one per histogram bin.
 * Each sets the flag once its own arguments have arrived. perfly waited
 * forever on an 0xa104 query in a captured run.
 * Approximated: the answer to indices 4 to 7 is the loaded context's
 * current colour, which is what the client reads them as; every other
 * answer is zero.
 */
static void test_hq_attribute_queries_set_flag_after_arguments(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, 0x00000210);
    gl_float(qts, 0.25f);
    gl_float(qts, 0.5f);
    gl_float(qts, 0.75f);
    gl_float(qts, 1);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    gl_word(qts, 0x0000a104);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    0);
    gl_word(qts, 5);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    g_assert_cmphex(qtest_readl(qts, REG_QUERY_ANSWER), ==, 0x3f000000);

    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    gl_word(qts, 0x00008b08);
    gl_word(qts, 0x100);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    0);
    gl_word(qts, 3);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    g_assert_cmphex(qtest_readl(qts, REG_QUERY_ANSWER), ==, 0);

    /* The arguments were consumed: the next word is a header again. */
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

/*
 * The current-attribute query routine reads 0xa104 indices 8 to 10 as
 * GL_CURRENT_NORMAL's floats, 11 to 14 as GL_CURRENT_TEXTURE_COORDS' and 15 as
 * GL_EDGE_FLAG, true when the answer has any of bits 0x01c00000. A new context
 * answers OpenGL's initial normal (0, 0, 1), texture coordinates (0, 0, 0, 1)
 * and edge flag true (2.7), and glNormal3f (0x070c) and glEdgeFlag (0x0f00
 * true, 0x1000 false) change the answers.
 */
static void test_hq_attribute_query_answers_normal_and_edge_flag(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_attribute_query(qts, 8), ==, 0);
    g_assert_cmphex(gl_attribute_query(qts, 9), ==, 0);
    g_assert_cmphex(gl_attribute_query(qts, 10), ==, 0x3f800000);
    g_assert_cmphex(gl_attribute_query(qts, 13), ==, 0);
    g_assert_cmphex(gl_attribute_query(qts, 14), ==, 0x3f800000);
    g_assert_cmphex(gl_attribute_query(qts, 15) & 0x01c00000, !=, 0);

    gl_three(qts, 0x0000070c, 0.5f, -1, 0.25f);
    gl_word(qts, 0x00001000);
    g_assert_cmphex(gl_attribute_query(qts, 8), ==, 0x3f000000);
    g_assert_cmphex(gl_attribute_query(qts, 9), ==, 0xbf800000);
    g_assert_cmphex(gl_attribute_query(qts, 10), ==, 0x3e800000);
    g_assert_cmphex(gl_attribute_query(qts, 15) & 0x01c00000, ==, 0);
    gl_word(qts, 0x00000f00);
    g_assert_cmphex(gl_attribute_query(qts, 15) & 0x01c00000, !=, 0);

    /* Each context keeps its own. */
    gl_word(qts, 0x00001000);
    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_attribute_query(qts, 8), ==, 0);
    g_assert_cmphex(gl_attribute_query(qts, 15) & 0x01c00000, !=, 0);
    gl_enter_slot(qts, GL_SLOT_A);
    g_assert_cmphex(gl_attribute_query(qts, 8), ==, 0x3f000000);
    g_assert_cmphex(gl_attribute_query(qts, 15) & 0x01c00000, ==, 0);
}

static void test_hq_flags_fence_token_sets_bit(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);


    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_PUSH_1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_PUSH_2);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                     MGRAS_HQ_FLAG_FENCE);


    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
}


static void test_hq_flags_fence_token_isolated(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                     MGRAS_HQ_FLAG_FENCE);
}

/*
 * A header's payload is
 * (len + 3) / 4 words. The IRIX 6.5.30 GL client writes glColor3ub as
 * 0x34800203 and one packed word, and glVertex3s as 0x39000006 and two
 * words. Each payload below would, if read as a header, claim eight bytes
 * and swallow the fence token that follows, so the fence bit shows whether
 * the payload was framed.
 */
static void mgras_fence_after_payload(uint32_t header, const uint32_t *payload,
                                      int words)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_CONTEXT_COMMAND, header);
    for (int i = 0; i < words; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, payload[i]);
    }
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

static void test_fifo_three_byte_payload_takes_one_word(void)
{
    /*
     * The client packs its three colour bytes above bit 8, so its own word
     * never has a length byte; the pipe must frame the word whatever it
     * holds.
     */
    static const uint32_t payload[] = { 0x00000008 };

    mgras_fence_after_payload(0x34800203, payload, ARRAY_SIZE(payload));
}

static void test_fifo_six_byte_payload_takes_two_words(void)
{
    /* glVertex3s(0, 8, 0): x << 16 | y, then z << 16. */
    static const uint32_t payload[] = { 0x00000008, 0x00000000 };

    mgras_fence_after_payload(0x39000006, payload, ARRAY_SIZE(payload));
}


static void test_command_accepts_hq_config_words(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x0000ff04 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00008c04 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x000c0004 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x000a0204 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080004 << 32) | 1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00000001 << 34) | 1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_context_write_accepted_after_completion(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writel(qts, REG_CONTEXT_COMMAND, 0xf200);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x80000008);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x40000);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==,
                    MGRAS_FINAL_TOKEN);
}


/*
 * The command-FIFO STATE-token header path reaches mgras_raster_rss_write()
 * the same way a direct MGRAS_COMMAND write does, so a fill it stages still
 * has to clear the rect-fill FILLMODE gate and read FILLCOLOR_R.
 */
static void test_command_fifo_state_write_headers(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_FILLMODE, 4) << 32) |
                 MGRAS_FILLMODE_RECT_FILL_VALUE);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_FILLCOLOR_R, 4) << 32) |
                 0x44);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_BLOCK_START, 4) << 32) |
                 ((60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_COLORMASKLSBSA, 4) << 32) |
                 0xffffffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_PP1FILLMODE, 4) << 32) |
                 ((uint32_t)MGRAS_LOGIC_OP_COPY << 26));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (61u << 16) | 61u));
    g_assert_cmphex(mgras_test_pixel(qts, 60, 60), ==, 0x44);
    g_assert_cmphex(mgras_test_pixel(qts, 61, 61), ==, 0x44);
    g_assert_cmphex(mgras_test_pixel(qts, 59, 59), ==, 0);
}


static void test_command_fifo_state_write_headers_not_yet_consumed(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_ALPHA, 4) << 32) | 0xff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_COLORMASKMSBS, 4) << 32) |
                 0x0000ffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_GLINE_XSTARTF, 4) << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_DRBPOINTERS, 4) << 32) | 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}

/*
 * Paint one point at (x, x) with the current PACKEDCOLOR and return the
 * pixel, so a test can show whether a discarded STATE write changed it.
 */
static uint32_t mgras_point_colour(unsigned int x)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (x << 16) | x));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (x << 16) | x));
    return mgras_test_pixel(qts, x, x);
}

/*
 * STATE headers the model does not execute are framed and skipped. A
 * 6-byte header's payload is two words, rounded up as for every header
 * with bit 31 clear; its payload words 0x8 would each claim eight bytes
 * if read as headers and swallow the fence after them. A 36-byte header
 * exceeds the payload buffer and is consumed whole. Neither changes
 * PACKEDCOLOR.
 */
static void test_command_fifo_skips_unaligned_and_oversized_state(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00112233)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xffffff));
    /* RGB format: RE_POINT reads PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_STATE_TOKEN(REG_PACKEDCOLOR, 6));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x8 << 32) | 0x8);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)MGRAS_FENCE_TOKEN << 32);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(mgras_point_colour(70), ==, 0x00112233);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_STATE_TOKEN(REG_PACKEDCOLOR, 36));
    for (int i = 0; i < 4; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     ((uint64_t)0xdeadbeef << 32) | 0xdeadbeef);
    }
    qtest_writeq(qts, REG_CONTEXT_COMMAND, (uint64_t)0xdeadbeef << 32);
    g_assert_cmphex(mgras_point_colour(72), ==, 0x00112233);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}


/*
 * Send a header for a class the model does not implement, followed by its
 * declared payload and a real header. The FIFO must consume the payload
 * and resume at the real header.
 *
 * The payload word below (0x00180004) is chosen so that, without the fix,
 * the parser reads it as a second bogus STATE header. That header then
 * consumes the next real header token as its payload, corrupting the
 * PACKEDCOLOR write that follows and making the corruption observable in
 * a paint. Any nonzero payload_bytes this class might carry has to be
 * consumed the same way.
 */
static void test_command_fifo_unrecognized_class_resyncs(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00020404 << 32) | 0x00180004);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_FILLMODE, 4) << 32) |
                 MGRAS_FILLMODE_RECT_FILL_VALUE);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_FILLCOLOR_R, 4) << 32) |
                 0x44);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_BLOCK_START, 4) << 32) |
                 ((62u << 16) | 62u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_COLORMASKLSBSA, 4) << 32) |
                 0xffffffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_PP1FILLMODE, 4) << 32) |
                 ((uint32_t)MGRAS_LOGIC_OP_COPY << 26));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(mgras_test_pixel(qts, 62, 62), ==, 0x44);
    g_assert_cmphex(mgras_test_pixel(qts, 63, 63), ==, 0x44);
    g_assert_cmphex(mgras_test_pixel(qts, 61, 61), ==, 0);
}


/*
 * RE_POINT paints at its own command value, not at BLOCKXYSTARTI: a stale
 * BLOCK_START must not steer the paint (see mgras_draw_point()).
 */
static void test_re_point_execute(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_POINT reads PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00abcdef)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (70u << 16) | 80u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 70, 80), ==, 0x00abcdef);
    g_assert_cmphex(mgras_test_pixel(qts, 10, 10), ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 71, 80), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 70, 81), ==, 0);
}

/*
 * A command-FIFO EXEC token's own declared payload is not evidenced (the
 * retained Toolchest capture drives RE_POINT only by direct form) and is
 * not read for position; it still has to be framed and consumed so it
 * cannot desync the parser or leak into the next command. Latch the pen
 * with a real RE_POINT state write first, then trigger it with a bounded
 * (8-byte) EXEC token carrying garbage payload words that must not reach
 * rss[] or the paint.
 */
static void test_re_point_execute_after_bounded_payload(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_POINT reads PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00556677)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_RE_POINT, 4) << 32) |
                 ((72u << 16) | 80u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_EXEC_TOKEN(REG_RE_POINT, 8) << 32) |
                 0xdeadbeef);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0xcafebabe << 32) | 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 72, 80), ==, 0x00556677);
}


/*
 * RE_LINE paints from the pen position RE_POINT last latched to RE_LINE's
 * own command value, not from BLOCKXYSTARTI to BLOCKXYENDI: a stale
 * BLOCK_START/BLOCK_END must not steer the draw (see mgras_draw_line()).
 */
static void test_re_line_execute(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_LINE reads PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00112233)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (1u << 16) | 1u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (2u << 16) | 2u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (100u << 16) | 100u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (105u << 16) | 105u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    g_assert_cmphex(mgras_test_pixel(qts, 100, 100), ==, 0x00112233);
    g_assert_cmphex(mgras_test_pixel(qts, 102, 102), ==, 0x00112233);
    g_assert_cmphex(mgras_test_pixel(qts, 105, 105), ==, 0x00112233);

    g_assert_cmphex(mgras_test_pixel(qts, 1, 1), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 105), ==, 0);
}

/*
 * A run of RE_LINEs chains: each one draws from the previous RE_POINT or
 * RE_LINE's own end point, not from the original RE_POINT every time.
 * A vertical leg from (10,10) to (10,20) followed by a
 * horizontal leg to (20,20) must start its second leg at (10,20), not
 * back at the original (10,10): a direct diagonal from (10,10) to
 * (20,20) would also light (15,15), which this checks stays unpainted.
 */
static void test_re_line_chains_from_re_point(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_LINE/RE_POINT read PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00998877)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (10u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (20u << 16) | 20u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 10, 10), ==, 0x00998877);
    g_assert_cmphex(mgras_test_pixel(qts, 10, 15), ==, 0x00998877);
    g_assert_cmphex(mgras_test_pixel(qts, 10, 20), ==, 0x00998877);
    g_assert_cmphex(mgras_test_pixel(qts, 15, 20), ==, 0x00998877);
    g_assert_cmphex(mgras_test_pixel(qts, 20, 20), ==, 0x00998877);

    g_assert_cmphex(mgras_test_pixel(qts, 15, 15), ==, 0);
}


/*
 * RE_POINT's CI8 ink is RED>>12&0xff (contract rule 4), not PACKEDCOLOR:
 * the reset default format is already CI8, so no PP1FILLMODE write is
 * needed here.
 */
static void test_re_point_ci8_reads_red_channel(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_RED, 0xab000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (70u << 16) | 80u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 70, 80), ==, 0xab);
    g_assert_cmphex(mgras_test_pixel(qts, 71, 80), ==, 0);
}


/*
 * RE_LINE's CI8 ink is RED>>12&0xff (contract rule 4), not PACKEDCOLOR:
 * the reset default format is already CI8, so no PP1FILLMODE write is
 * needed here.
 */
static void test_re_line_ci8_reads_red_channel(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_RED, 0xcd000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (100u << 16) | 100u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (105u << 16) | 105u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 100, 100), ==, 0xcd);
    g_assert_cmphex(mgras_test_pixel(qts, 102, 102), ==, 0xcd);
    g_assert_cmphex(mgras_test_pixel(qts, 105, 105), ==, 0xcd);

    g_assert_cmphex(mgras_test_pixel(qts, 100, 105), ==, 0);
}


/*
 * FILLMODE 0x60 draws an opaque stippled line: a set bit of the 0x15a
 * pattern paints the line colour and a clear bit the BKGRD_RG/BKGRD_BA
 * background. The pattern starts at the line's first pixel with bit 31,
 * not at screen x % 32 == 0. The IRIX 6.5.30 greeter draws its icon
 * bodies this way (see MGRAS_RSS_LINE_STIPPLE in
 * hw/misc/sgi-mgras-raster.c). The line starts at an odd x so that
 * screen-aligned indexing would swap every pixel, and the background's
 * three channels differ so that a swapped channel shows.
 */
static void test_re_line_opaque_stipple_starts_at_line(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) |
                               MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00aabbcc)));
    /* Background (0x12, 0x34, 0x56) in the server's 12-bit channel form. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_RG, (0x12u << 4) | (0x34u << 16)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_BA, 0x56u << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_STIPPLE_OPAQUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_LINE_STIPPLE, 0xaaaaaaaa));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RE_POINT, (33u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (36u << 16) | 10u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    /* 0xaaaaaaaa sets the line's first, third, ... pixels. */
    g_assert_cmphex(mgras_test_pixel(qts, 33, 10), ==, 0x00aabbcc);
    g_assert_cmphex(mgras_test_pixel(qts, 34, 10), ==, 0x00123456);
    g_assert_cmphex(mgras_test_pixel(qts, 35, 10), ==, 0x00aabbcc);
    g_assert_cmphex(mgras_test_pixel(qts, 36, 10), ==, 0x00123456);
    g_assert_cmphex(mgras_test_pixel(qts, 37, 10), ==, 0);
}

/*
 * FILLMODE 0x20 without 0x40 is transparent: a clear pattern bit leaves
 * the pixel alone. The greeter's text cursor is drawn this way over the
 * name field, as a 2-pixel stem from pattern 0x30c30c30. Under CI8 the
 * line colour is RED's index, and a stale BKGRD_RG must not show.
 */
static void test_re_line_transparent_stipple_keeps_clear_bits(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* A solid CI8 line of index 0x16 under the stippled one. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_RED, 0x16000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RE_POINT, (454u << 16) | 700u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (459u << 16) | 700u));

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_RED, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_BKGRD_RG, 0x14));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_STIPPLE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_LINE_STIPPLE, 0x30c30c30));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_RE_POINT, (454u << 16) | 700u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (459u << 16) | 700u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    /* The six pixels read pattern bits 31:26, 0b001100. */
    g_assert_cmphex(mgras_test_pixel(qts, 454, 700), ==, 0x16);
    g_assert_cmphex(mgras_test_pixel(qts, 455, 700), ==, 0x16);
    g_assert_cmphex(mgras_test_pixel(qts, 456, 700), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 457, 700), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 458, 700), ==, 0x16);
    g_assert_cmphex(mgras_test_pixel(qts, 459, 700), ==, 0x16);
}


/*
 * A pixel outside the clip box (clip_y_rows() in sgi-mgras-test.h) is not
 * written, so a fill that covers the box paints only the box. Without
 * this the Icon Catalog's icons painted through the Background Setting
 * panel stacked above them.
 */
static void test_fill_honours_clip_rectangle(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x66));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    /* Box x 110-119, y 210-219, inclusive. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_X, (110u << 16) | 119u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_Y, clip_y_rows(210, 219)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_MODE, MGRAS_CLIP_MODE_ON));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (100u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (129u << 16) | 229u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 110, 210), ==, 0x66);
    g_assert_cmphex(mgras_test_pixel(qts, 119, 219), ==, 0x66);
    g_assert_cmphex(mgras_test_pixel(qts, 109, 215), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 120, 215), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 115, 209), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 115, 220), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 200), ==, 0);

    /* 0x14f back to 0 lifts the clip: the same fill paints everywhere. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_CLIP_MODE, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (129u << 16) | 229u));
    g_assert_cmphex(mgras_test_pixel(qts, 100, 200), ==, 0x66);
    g_assert_cmphex(mgras_test_pixel(qts, 129, 229), ==, 0x66);
}

/*
 * Overlay-form draws (DRBPOINTERS 0x101c0, COLORMASKMSBS bits 6:4) run
 * inside the same per-box loop: 1,709 clipped executes in the desktop
 * session are overlay-form, and every one meets its box. The overlay
 * index is dropped outside the box like a colour write.
 */
static void test_overlay_fill_honours_clip_rectangle(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 0x000101c0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKMSBS, 0x70));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x5));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_X, (110u << 16) | 119u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_Y, clip_y_rows(210, 219)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_MODE, MGRAS_CLIP_MODE_ON));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (100u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (129u << 16) | 229u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_overlay(qts, 110, 210), ==, 0x5);
    g_assert_cmphex(mgras_test_overlay(qts, 119, 219), ==, 0x5);
    g_assert_cmphex(mgras_test_overlay(qts, 109, 215), ==, 0);
    g_assert_cmphex(mgras_test_overlay(qts, 115, 220), ==, 0);
    g_assert_cmphex(mgras_test_overlay(qts, 100, 200), ==, 0);
}

/*
 * The box limits a fill whatever its op and colour mask, and only the
 * pixels inside take the fill's format tag. The seed is an RGB fill of
 * 0x0f0f0f over two rows; the box covers four columns of the second.
 */
static void test_masked_fill_honours_clip_rectangle(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) |
                               MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x0f << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_G, 0x0f << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_B, 0x0f << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (300u << 16) | 400u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (309u << 16) | 401u));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_X, (302u << 16) | 305u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_Y, clip_y_rows(401, 401)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_MODE, MGRAS_CLIP_MODE_ON));

    /* White XORed over the seed is 0xf0f0f0; green keeps the seed. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ff00ff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_XOR << 26) |
                               MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xff << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_G, 0xff << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_B, 0xff << 4));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (309u << 16) | 401u));

    g_assert_cmphex(mgras_test_pixel(qts, 302, 401), ==, 0x00f00ff0);
    g_assert_cmphex(mgras_test_pixel(qts, 305, 401), ==, 0x00f00ff0);
    g_assert_cmphex(mgras_test_pixel(qts, 301, 401), ==, 0x000f0f0f);
    g_assert_cmphex(mgras_test_pixel(qts, 306, 401), ==, 0x000f0f0f);
    g_assert_cmphex(mgras_test_pixel(qts, 303, 400), ==, 0x000f0f0f);

    /* A CI8 copy under the low byte alone: index and tag, in the box. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x000000ff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x5a));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (309u << 16) | 401u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 302, 401), ==, 0x00f00f5a);
    g_assert_cmphex(mgras_test_format(qts, 302, 401), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_test_format(qts, 305, 401), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_test_pixel(qts, 301, 401), ==, 0x000f0f0f);
    g_assert_cmphex(mgras_test_format(qts, 301, 401), ==,
                    MGRAS_PIXEL_FORMAT_RGB);
    g_assert_cmphex(mgras_test_format(qts, 306, 401), ==,
                    MGRAS_PIXEL_FORMAT_RGB);
    g_assert_cmphex(mgras_test_format(qts, 303, 400), ==,
                    MGRAS_PIXEL_FORMAT_RGB);
}

/*
 * A glyph row is clipped column by column, and a row outside the box
 * still takes its execute: the next execute paints the next row. Rows
 * 200-203 of a 32-column glyph at x 100; the box is x 110-119 on rows
 * 201 and 202. Row 201's pattern sets columns 16-23 only, so it shows
 * whether row 200's execute was counted.
 */
static void test_glyph_row_honours_clip_rectangle(void)
{
    static const uint32_t rows[4] = {
        0xffffffff, 0x0000ff00, 0xffffffff, 0xffffffff,
    };

    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) |
                               MGRAS_PP1FILLMODE_RGB));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00ffffff)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_X, (110u << 16) | 119u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_Y, clip_y_rows(201, 202)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_MODE, MGRAS_CLIP_MODE_ON));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (100u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (131u << 16) | 203u));
    for (int i = 0; i < ARRAY_SIZE(rows); i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_CHAR_H, rows[i]));
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 115, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 115, 201), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 116, 201), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 119, 201), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 120, 201), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 109, 202), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 110, 202), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 119, 202), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 120, 202), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 115, 203), ==, 0);
}

/*
 * The X server draws overlay text as glyph rows in the overlay form with
 * the colour mask at 0: the overlay index is written wherever a glyph bit
 * is set, and the colour planes are left alone. Only the colour write is
 * gated by the colour mask; the overlay write follows the glyph bits and
 * the box. Rows 300 and 301 of a 16-column glyph at x 400, index 0x3.
 */
static void test_overlay_glyph_row_under_zero_colour_mask(void)
{
    static const uint32_t rows[2] = { 0xa5000000, 0x0f000000 };

    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 0x000101c0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKMSBS, 0x70));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, MGRAS_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_RED, 0x3000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (400u << 16) | 300u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (415u << 16) | 301u));
    for (int i = 0; i < ARRAY_SIZE(rows); i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_CHAR_H, rows[i]));
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    /* Row 300 is 1010 0101: set bits take the index, clear ones stay 0. */
    g_assert_cmphex(mgras_test_overlay(qts, 400, 300), ==, 0x3);
    g_assert_cmphex(mgras_test_overlay(qts, 401, 300), ==, 0);
    g_assert_cmphex(mgras_test_overlay(qts, 402, 300), ==, 0x3);
    g_assert_cmphex(mgras_test_overlay(qts, 405, 300), ==, 0x3);
    g_assert_cmphex(mgras_test_overlay(qts, 407, 300), ==, 0x3);
    g_assert_cmphex(mgras_test_overlay(qts, 408, 300), ==, 0);
    /* Row 301 is 0000 1111. */
    g_assert_cmphex(mgras_test_overlay(qts, 403, 301), ==, 0);
    g_assert_cmphex(mgras_test_overlay(qts, 404, 301), ==, 0x3);
    g_assert_cmphex(mgras_test_overlay(qts, 407, 301), ==, 0x3);
    /* The zero colour mask leaves the colour planes untouched. */
    g_assert_cmphex(mgras_test_pixel(qts, 400, 300), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 404, 301), ==, 0);
}

/*
 * The box is in screen coordinates: a line placed by XYWIN is clipped
 * where it lands, not where its raw coordinates point. The desktop session
 * this came from drew 3,617 clipped window-relative lines that meet their
 * box only after the origin is applied.
 */
static void test_re_line_clip_follows_xywin_origin(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00112233)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_XYWIN, (1023u - 300u) << 16 | 200u));
    /* Screen x 205-209 on screen row 305. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_X, (205u << 16) | 209u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_Y, clip_y_rows(305, 305)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_CLIP_MODE, MGRAS_CLIP_MODE_ON));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (0u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (20u << 16) | 5u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 204, 305), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 205, 305), ==, 0x00112233);
    g_assert_cmphex(mgras_test_pixel(qts, 209, 305), ==, 0x00112233);
    g_assert_cmphex(mgras_test_pixel(qts, 210, 305), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 200, 305), ==, 0);
}

/*
 * XYWIN (0x115) supplies a window origin used by RE_POINT and RE_LINE.
 * This test checks that the model applies it to the drawing coordinates.
 */
static void test_re_point_execute_applies_xywin_origin(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_LINE/RE_POINT read PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00abcdef)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_XYWIN, (200u << 16) | 300u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (70u << 16) | 80u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    /*
     * XYWIN's y field (200 here) is packed top-down; the origin applies
     * it as s->height - 1 - oy, not oy directly.
     */
    g_assert_cmphex(mgras_test_pixel(qts, 70 + 300, 80 + (1023 - 200)), ==,
                     0x00abcdef);
    g_assert_cmphex(mgras_test_pixel(qts, 70, 80), ==, 0);
}

static void test_re_line_execute_applies_xywin_origin(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_LINE/RE_POINT read PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00112233)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_XYWIN, (200u << 16) | 300u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (100u << 16) | 100u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (105u << 16) | 105u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    /*
     * XYWIN's y field (200 here) is packed top-down; the origin applies
     * it as s->height - 1 - oy, not oy directly.
     */
    g_assert_cmphex(mgras_test_pixel(qts, 100 + 300, 100 + (1023 - 200)), ==,
                     0x00112233);
    g_assert_cmphex(mgras_test_pixel(qts, 105 + 300, 105 + (1023 - 200)), ==,
                     0x00112233);

    g_assert_cmphex(mgras_test_pixel(qts, 100, 100), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 105, 105), ==, 0);
}

/*
 * A live greeter capture's dominant XYWIN value packs a zero x field with
 * a nonzero y field (0x03ff0000: x field 0, y field 1023); 1,456 of its
 * 1,953 RE_LINE executes carry it and already paint at their correct
 * absolute position. Adding the y field unconditionally would push those
 * already-correct lines off-window, so the origin must stay gated on the
 * x field alone, not on the whole register being nonzero.
 */
static void test_re_line_execute_ignores_zero_x_field_xywin(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_LINE/RE_POINT read PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00445566)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_XYWIN, (1023u << 16) | 0u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (100u << 16) | 100u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (105u << 16) | 105u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 100, 100), ==, 0x00445566);
    g_assert_cmphex(mgras_test_pixel(qts, 105, 105), ==, 0x00445566);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 100 + 1023), ==, 0);
}


/*
 * Nothing masks XYWIN's y field to s->height - 1 the way the driver
 * always writes it: a guest value past 1023 drives
 * mgras_apply_window_origin()'s "(s->height - 1) - oy" term
 * negative. Before the fix, RE_POINT's own "x >= s->width || y >=
 * s->height" rejection let that through -- both sides of the OR are
 * false for a negative int compared against an unsigned width -- and
 * mgras_put_pixel() took the resulting negative index in a parameter
 * declared unsigned, turning it into a multi-gigabyte forward offset into
 * s->front. This asserts the point is rejected instead: no crash, no
 * sticky error, and the screen unchanged.
 */
static void test_re_point_execute_rejects_out_of_range_xywin_origin(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_POINT reads PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00abcdef)));
    /*
     * y field (5000) is far past the 1023 the driver ever writes; x field
     * nonzero (1) is what gates mgras_apply_window_origin()'s add.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_XYWIN, (5000u << 16) | 1u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (0u << 16) | 0u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 1, 0), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The same out-of-range XYWIN origin reaches RE_LINE through
 * mgras_apply_window_origin(), applied to both endpoints, and the line
 * loop's own "x0 < s->width && y0 < s->height" plot check has
 * the identical gap. The first RE_POINT here keeps XYWIN at its x-field-0
 * reset value so it safely latches the pen position before the bad XYWIN
 * below is installed, isolating RE_LINE's own check from RE_POINT's.
 */
static void test_re_line_execute_rejects_out_of_range_xywin_origin(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_LINE/RE_POINT read PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                               (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00112233)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (0u << 16) | 0u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 0, 0), ==, 0x00112233);

    /*
     * y field (5000) is far past the 1023 the driver ever writes; x field
     * nonzero (1) is what gates mgras_apply_window_origin()'s add.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_XYWIN, (5000u << 16) | 1u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (5u << 16) | 5u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 5, 5), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 6, 5), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_public_fifo_fill(void)
{
    mgras_reset();
    mgras_public_setup(qts, false);
    mgras_public_write(qts, 0x5b, 0x00debc9a, false);
    mgras_public_rect(qts, 30, 42, 5, 4, true);
    mgras_public_write(qts, 0x5b, 0x00332211, false);
    mgras_public_rect(qts, 31, 43, 3, 2, false);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0x9abcde);
    mgras_public_write(qts, 0x45, 0x18, true);
    for (unsigned y = 42; y <= 45; y++) {
        for (unsigned x = 30; x <= 34; x++) {
            bool covered = x >= 31 && x <= 33 && y >= 43 && y <= 44;

            g_assert_cmphex(mgras_test_pixel(qts, x, y), ==,
                            covered ? 0x112233 : 0x9abcde);
            g_assert_cmphex(mgras_test_wid(qts, x, y), ==, 0);
        }
    }
    mgras_public_write(qts, 0x163, 0x0000ff, false);
    mgras_public_write(qts, 0x5b, 0x00665544, false);
    mgras_public_write(qts, 0x45, 0x18, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0x112266);
}

static void test_public_fifo_xor(void)
{
    mgras_reset();
    mgras_public_setup(qts, false);
    for (unsigned i = 0; i < 6; i++) {
        mgras_public_write(qts, 0x5b, MGRAS_PIXEL_WORD(0x102030 + i), false);
        mgras_public_rect(qts, 31 + i % 3, 43 + i / 3, 1, 1, true);
    }
    mgras_public_write(qts, 0x161, 0x18006304, false);
    mgras_public_write(qts, 0x5b, 0x005a3cc3, false);
    mgras_public_rect(qts, 31, 43, 3, 2, true);
    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(mgras_test_pixel(qts, 31 + i % 3, 43 + i / 3), ==,
                        (0x102030 + i) ^ 0xc33c5a);
    }
    mgras_public_write(qts, 0x45, 0x18, true);
    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(mgras_test_pixel(qts, 31 + i % 3, 43 + i / 3), ==,
                        0x102030 + i);
    }
}

static void test_public_fifo_glyph_narrow(void)
{
    mgras_reset();
    mgras_public_setup(qts, true);
    mgras_public_rect(qts, 31, 43, 5, 2, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x70, 0x88000000, true);
    mgras_public_write(qts, 0x70, 0x50000000, true);
    mgras_public_write(qts, 0x70, 0xff000000, true);
    for (unsigned y = 42; y <= 45; y++) {
        for (unsigned x = 30; x <= 36; x++) {
            uint32_t expected = 0;

            if (x >= 31 && x <= 35 && y >= 43 && y <= 44) {
                uint8_t row = y == 43 ? 0x88 : 0x50;
                expected = row & (0x80 >> (x - 31)) ? 0x112233 : 0xa0b0d0;
            }
            g_assert_cmphex(mgras_test_pixel(qts, x, y), ==, expected);
        }
    }
    mgras_public_rect(qts, 40, 43, 5, 1, true);
    mgras_public_write(qts, 0x70, 0xff000000, true);
    for (unsigned x = 40; x < 45; x++) {
        g_assert_cmphex(mgras_test_pixel(qts, x, 43), ==, 0x112233);
    }
    mgras_public_rect(qts, 40, 44, 5, 1, true);
    mgras_public_write(qts, 0x70, 0, true);
    mgras_public_setup(qts, false);
    mgras_public_write(qts, 0x5b, 0x00d0b0a0, false);
    mgras_public_rect(qts, 40, 45, 5, 1, true);
    g_assert_cmphex(mgras_test_pixel(qts, 40, 44), ==,
                    mgras_test_pixel(qts, 40, 45));
}

static void test_public_fifo_glyph_wide(void)
{
    mgras_reset();
    mgras_public_setup(qts, true);
    mgras_public_rect(qts, 31, 43, 12, 1, true);
    mgras_public_write(qts, 0x71, 0x24000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x70, 0x81240000, false);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x71, 0x24000000, true);
    for (unsigned i = 0; i < 14; i++) {
        uint32_t expected = i >= 12 ? 0 :
                            (i == 0 || i == 7 || i == 10 ? 0x112233 : 0xa0b0d0);
        g_assert_cmphex(mgras_test_pixel(qts, 31 + i, 43), ==, expected);
    }
    mgras_public_rect(qts, 31, 44, 12, 1, true);
    mgras_public_write(qts, 0x71, 0xff000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 44), ==, 0);
    mgras_public_write(qts, 0x70, 0xffff0000, false);
    mgras_public_write(qts, 0x71, 0xff000000, true);
    for (unsigned i = 0; i < 12; i++) {
        g_assert_cmphex(mgras_test_pixel(qts, 31 + i, 44), ==, 0x112233);
    }
}

static void test_public_fifo_glyph_mask(void)
{
    mgras_reset();
    mgras_public_setup(qts, false);
    mgras_public_write(qts, 0x5b, 0x008d6e4f, false);
    mgras_public_rect(qts, 30, 43, 7, 1, true);
    mgras_public_setup(qts, true);
    mgras_public_write(qts, 0x163, 0x00ff00, false);
    mgras_public_rect(qts, 31, 43, 5, 1, true);
    mgras_public_write(qts, 0x70, 0x88000000, true);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmphex(mgras_test_pixel(qts, 31 + i, 43), ==,
                        i == 0 || i == 4 ? 0x4f228d : 0x4fb08d);
        g_assert_cmphex(mgras_test_wid(qts, 31 + i, 43), ==, 0);
    }
    g_assert_cmphex(mgras_test_pixel(qts, 30, 43), ==, 0x4f6e8d);
    g_assert_cmphex(mgras_test_pixel(qts, 36, 43), ==, 0x4f6e8d);
}

static void test_public_fifo_glyph_validity(void)
{
    mgras_reset();
    mgras_public_setup(qts, true);
    mgras_public_rect(qts, 31, 43, 12, 1, true);
    mgras_public_write(qts, 0x70, 0x81240000, false);
    mgras_public_write(qts, 0x47, (42 << 16) | 43, false);
    mgras_public_write(qts, 0x71, 0x24000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x45, 0x18, true);
    mgras_public_write(qts, 0x71, 0x24000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x70, 0x81240000, false);
    mgras_public_write(qts, 0x110, 0x00400018, false);
    mgras_public_write(qts, 0x45, 0x18, true);
    mgras_public_write(qts, 0x71, 0x24000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x70, 0x81240000, false);
    mgras_reset();
    mgras_public_setup(qts, true);
    mgras_public_rect(qts, 31, 43, 12, 1, true);
    mgras_public_write(qts, 0x71, 0x24000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_rect(qts, 31, 43, 5, 2, true);
    mgras_reset();
    mgras_public_write(qts, 0x70, 0xff000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
}

static void test_public_fifo_unsupported(void)
{
    mgras_reset();
    mgras_public_setup(qts, false);
    mgras_public_rect(qts, 31, 43, 3, 2, false);
    mgras_public_write(qts, 0x45, 0x19, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x110, 1, false);
    mgras_public_write(qts, 0x45, 0x18, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x110, 0, false);
    mgras_public_write(qts, 0x161, 0x0c006301, false);
    mgras_public_write(qts, 0x45, 0x18, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x161, 0x0c006300, false);
    mgras_public_write(qts, 0x47, (30 << 16) | 44, false);
    mgras_public_write(qts, 0x45, 0x18, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_write(qts, 0x47, (1280 << 16) | 44, false);
    mgras_public_write(qts, 0x45, 0x18, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_setup(qts, true);
    mgras_public_rect(qts, 31, 43, 17, 1, true);
    mgras_public_write(qts, 0x70, 0xffff0000, false);
    mgras_public_write(qts, 0x71, 0xff000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_rect(qts, 31, 43, 12, 2, true);
    mgras_public_write(qts, 0x70, 0xffff0000, false);
    mgras_public_write(qts, 0x71, 0xff000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_rect(qts, 31, 43, 5, 1, true);
    mgras_public_write(qts, 0x71, 0xff000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    mgras_public_rect(qts, 31, 43, 12, 1, true);
    mgras_public_write(qts, 0x70, 0xffff0000, true);
    mgras_public_write(qts, 0x71, 0xff000000, true);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    qtest_writeq(qts, MGRAS_REG(0x020400), 0x001c450800000018ULL);
    g_assert_cmphex(mgras_test_pixel(qts, 31, 43), ==, 0);
    qtest_writel(qts, MGRAS_REG(0x020400), 0x001c4504);
    mgras_assert_width_error(qts, MGRAS_REG(0x020400));
}

static void test_public_fifo_unsupported_execute_selectors(void)
{
    static const struct {
        uint64_t packet;
        uint32_t mode;
    } cases[] = {
        { 0x001d410400000d00ULL, 0 },
        { 0x001c4004001f002bULL, 0 },
        { 0x001c41040020002bULL, 0 },
        { 0x001c47040020002bULL, 0x00100000 },
        { 0x001c7004ff000000ULL, 0x10400008 },
        { 0x001c7104ff000000ULL, 0x10400008 },
        { 0x001c7104ff000000ULL, 0x10c00000 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        mgras_reset();
        mgras_public_setup(qts, false);
        mgras_public_rect(qts, 31, 43, 2, 1, false);
        mgras_public_write(qts, 0x110, cases[i].mode, false);
        mgras_public_write(qts, 0x40, (31 << 16) | 43, false);
        mgras_public_write(qts, 0x70, 0xff000000, false);
        qtest_writeq(qts, MGRAS_REG(0x020400), cases[i].packet);
        for (unsigned y = 42; y <= 44; y++) {
            for (unsigned x = 30; x <= 33; x++) {
                g_assert_cmphex(mgras_test_pixel(qts, x, y), ==, 0);
            }
        }
    }
}

static void test_public_fifo_glyph_widths(void)
{
    static const unsigned widths[] = { 1, 8, 9, 16 };

    mgras_reset();
    mgras_public_setup(qts, true);
    for (unsigned j = 0; j < G_N_ELEMENTS(widths); j++) {
        unsigned width = widths[j];

        mgras_public_rect(qts, 31, 43 + j, width, 1, true);
        if (width <= 8) {
            mgras_public_write(qts, 0x70, 0x81000000, false);
            g_assert_cmphex(mgras_test_pixel(qts, 31, 43 + j), ==, 0);
            mgras_public_write(qts, 0x70, 0x81000000, true);
        } else {
            mgras_public_write(qts, 0x70, 0x81810000, false);
            mgras_public_write(qts, 0x71, 0x81000000, true);
        }
        for (unsigned i = 0; i <= width; i++) {
            uint32_t expected = i == width ? 0 :
                (i % 8 == 0 || i % 8 == 7 ? 0x112233 : 0xa0b0d0);
            g_assert_cmphex(mgras_test_pixel(qts, 31 + i, 43 + j), ==,
                            expected);
        }
    }
}

static void test_re_char_h_execute(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: a glyph row reads PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00ffffff)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (207u << 16) | 207u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_CHAR_H, 4) << 32) |
                 ((uint32_t)0x18 << 24));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)MGRAS_EXEC_TOKEN(REG_CHAR_H, 0) << 32);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    g_assert_cmphex(mgras_test_pixel(qts, 200, 200), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 203, 200), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 204, 200), ==, 0x00ffffff);
}

static void test_fifo_re_blockcopy_unsupported(void)
{
    uint32_t native_before, wid_before, display_before, surface_before;

    mgras_reset();

    mgras_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 0x56, 0x12345600);

    /* A real rect fill establishes native_before at the CI8 index above. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x56));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (50u << 16) | 50u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (50u << 16) | 50u));
    native_before = mgras_test_pixel(qts, 50, 50);
    wid_before = mgras_test_wid(qts, 50, 50);
    display_before = mgras_test_display_pixel(qts, 50, 50);
    surface_before = mgras_screendump_pixel(qts, 50, 50);
    g_assert_cmphex(native_before, ==, 0x56);
    g_assert_cmphex(wid_before, ==, 0);
    g_assert_cmphex(display_before, ==, surface_before);

    /* A source rectangle makes the rejected copy request meaningful. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x21));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (11u << 16) | 11u));
    g_assert_cmphex(mgras_test_pixel(qts, 10, 10), ==, 0x21);

    /* The bounded parser accepts the payload but does not execute 0x045. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_EXEC_TOKEN(REG_RE_BLOCKCOPY, 0x18));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)((10u << 16) | 10u) << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(mgras_test_pixel(qts, 50, 50), ==, native_before);
    g_assert_cmphex(mgras_test_wid(qts, 50, 50), ==, wid_before);
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 50), ==, display_before);
    g_assert_cmphex(mgras_screendump_pixel(qts, 50, 50), ==, surface_before);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    /*
     * Parser continuation still works after the unsupported submission.
     * RGB format: RE_POINT reads PACKEDCOLOR only under this format.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00abcdef)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_RE_POINT, 4) << 32) |
                 ((90u << 16) | 90u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)MGRAS_EXEC_TOKEN(REG_RE_POINT, 0) << 32);
    g_assert_cmphex(mgras_test_pixel(qts, 90, 90), ==, 0x00abcdef);

    /* An incomplete FIFO submission must not survive reset. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_EXEC_TOKEN(REG_RE_BLOCKCOPY, 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)((10u << 16) | 10u) << 32));
    qtest_system_reset(qts);
    mgras_enter_context(qts);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00abcdef)));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_RE_POINT, 4) << 32) |
                 ((90u << 16) | 90u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)MGRAS_EXEC_TOKEN(REG_RE_POINT, 0) << 32);
    g_assert_cmphex(mgras_test_pixel(qts, 90, 90), ==, 0x00abcdef);
}
int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);


    mgras_qtest_add_func(
        "sn0/origin2000/mgras/rgb-pixel-words-read-back-unchanged",
        test_rgb_pixel_words_read_back_unchanged, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "packedcolor-matches-fillcolor-channels",
        test_packedcolor_matches_fillcolor_channels, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/ge-upload-readback",
        test_ge_upload_and_readback, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/ge-upload-slots-independent",
        test_ge_upload_slots_do_not_cross_contaminate, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/ge-upload-rearm-rereads",
        test_ge_upload_rearm_rereads_from_start, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/prefix-register-reads-back",
        test_prefix_register_reads_back, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/prefix-rewrite-after-entry-selection",
        test_prefix_rewrite_after_entry_selection_does_not_poison_context,
        true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/context-recognizer-ordering",
        test_context_recognizer_ordering, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/second-switch-restarts-word-count",
        test_second_context_switch_restarts_word_count, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/entry-requires-prefix",
        test_entry_requires_prefix, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/context-command-after-image",
        test_context_command_after_image_is_ordinary_token, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fifo-occupancy-always-drained",
        test_fifo_occupancy_always_drained, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-fill-and-reset",
        test_direct_fill_and_reset, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/logic-op-truth-table",
        test_logic_op_truth_table, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/logic-op-xor-twice-restores-rect",
        test_logic_op_xor_twice_restores_rect, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/logic-op-xor-twice-restores-line",
        test_logic_op_xor_twice_restores_line, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/logic-op-xor-twice-restores-span",
        test_logic_op_xor_twice_restores_span, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fill-mask-reads-colormasklsbsa",
        test_fill_mask_reads_colormasklsbsa, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/wid-from-display-id-table",
        test_wid_from_display_id_table, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fill-color-reads-fillcolor-registers",
        test_fill_color_reads_fillcolor_registers, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/ir-alias-execute-paints-nothing",
        test_ir_alias_execute_paints_nothing, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-glyph-row",
        test_direct_glyph_row, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/glyph-row-before-any-context-switch",
        test_glyph_row_before_any_context_switch, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/pp1-format-field-5-is-ci8",
        test_pp1_format_field_5_is_ci8, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/pp1-bit-25-replaces",
        test_pp1_bit_25_replaces, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/prom-ink-fill-in-hardware-rows",
        test_prom_ink_fill_in_hardware_rows, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/prom-glyph-rows-in-hardware-rows",
        test_prom_glyph_rows_in_hardware_rows, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/prom-wide-glyph-row-on-char-l",
        test_prom_wide_glyph_row_on_char_l, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/prom-glyph-rows-top-down-with-config-bit-3",
        test_prom_glyph_rows_top_down_with_config_bit_3, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/prom-ink-fill-clips-past-the-top",
        test_prom_ink_fill_clips_past_the_top, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/prom-block-other-fillmode-is-inert",
        test_prom_block_other_fillmode_is_inert, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-glyph-row-char-h-only",
        test_direct_glyph_row_char_h_only, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-glyph-row-char-l-execute",
        test_direct_glyph_row_char_l_execute, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "direct-glyph-row-ci8-reads-red-channel",
        test_direct_glyph_row_ci8_reads_red_channel, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-byte-row",
        test_direct_byte_row, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "direct-byte-row-pitch-from-padding-field",
        test_direct_byte_row_pitch_from_padding_field, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-byte-row-pairs",
        test_direct_byte_row_pairs, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-byte-row-pairs-row-pad-bits",
        test_direct_byte_row_pairs_row_pad_bits, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-byte-row-with-row-pad-bits",
        test_direct_byte_row_with_row_pad_bits, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/direct-byte-row-seed-packing",
        test_direct_byte_row_seed_packing, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/display-surface-conversion",
        test_display_surface_conversion, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/reset-refreshes-display-surface",
        test_reset_refreshes_display_surface, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/pseudocolor-scanout-looks-up-cmap",
        test_pseudocolor_scanout_looks_up_cmap, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/cmap-high-byte-packing",
        test_cmap_high_byte_packing, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "pixel-format-tag-resolves-independently",
        test_pixel_format_tag_resolves_independently, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/rgb12-fill-packs-four-bit-channels",
        test_rgb12_fill_packs_four_bit_channels, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/rgb12-line-and-glyph-take-top-nibbles",
        test_rgb12_line_and_glyph_take_top_nibbles, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/rgb12-pixel-word-reads-red-low",
        test_rgb12_pixel_word_reads_red_low, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/target-first-cmap-visual-selection",
        test_target_first_cmap_visual_selection, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/masked-out-paint-keeps-the-visual",
        test_masked_out_paint_keeps_the_visual, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/paint-variants-refresh-the-surface",
        test_paint_variants_refresh_the_surface, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "cmap-selector-addresses-the-right-slot",
        test_cmap_selector_addresses_the_right_slot, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/cmap-selector-start-entry",
        test_cmap_selector_start_entry, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fifo-lowater-interrupt",
        test_fifo_lowater_interrupt, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fifo-threshold-registers",
        test_fifo_threshold_registers, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "hq-attribute-queries-set-flag-after-arguments",
        test_hq_attribute_queries_set_flag_after_arguments, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "hq-attribute-query-answers-normal-and-edge-flag",
        test_hq_attribute_query_answers_normal_and_edge_flag, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/hq-flags-set-and-clear",
        test_hq_flags_set_and_clear, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/hq-flags-fence-token-sets-bit",
        test_hq_flags_fence_token_sets_bit, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/hq-flags-fence-token-isolated",
        test_hq_flags_fence_token_isolated, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fifo-3-byte-payload-is-one-word",
        test_fifo_three_byte_payload_takes_one_word, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fifo-6-byte-payload-is-two-words",
        test_fifo_six_byte_payload_takes_two_words, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/sched-arg-4-round-trip",
        test_sched_arg_4_round_trip, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/command-accepts-hq-config-words",
        test_command_accepts_hq_config_words, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/context-write-after-completion",
        test_context_write_accepted_after_completion, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/command-fifo-state-write-headers",
        test_command_fifo_state_write_headers, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "command-fifo-state-write-headers-not-yet-consumed",
        test_command_fifo_state_write_headers_not_yet_consumed, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "command-fifo-skips-unaligned-and-oversized-state",
        test_command_fifo_skips_unaligned_and_oversized_state, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "command-fifo-unrecognized-class-resyncs",
        test_command_fifo_unrecognized_class_resyncs, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-point-execute",
        test_re_point_execute, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "re-point-execute-after-bounded-payload",
        test_re_point_execute_after_bounded_payload, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-line-execute",
        test_re_line_execute, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-line-chains-from-re-point",
        test_re_line_chains_from_re_point, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-point-ci8-reads-red-channel",
        test_re_point_ci8_reads_red_channel, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-line-ci8-reads-red-channel",
        test_re_line_ci8_reads_red_channel, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-line-opaque-stipple-starts-at-line",
        test_re_line_opaque_stipple_starts_at_line, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "re-line-transparent-stipple-keeps-clear-bits",
        test_re_line_transparent_stipple_keeps_clear_bits, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-point-execute-applies-xywin-origin",
        test_re_point_execute_applies_xywin_origin, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-line-execute-applies-xywin-origin",
        test_re_line_execute_applies_xywin_origin, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "re-line-execute-ignores-zero-x-field-xywin",
        test_re_line_execute_ignores_zero_x_field_xywin, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "re-point-execute-rejects-out-of-range-xywin-origin",
        test_re_point_execute_rejects_out_of_range_xywin_origin, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/"
        "re-line-execute-rejects-out-of-range-xywin-origin",
        test_re_line_execute_rejects_out_of_range_xywin_origin, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fill-honours-clip-rectangle",
        test_fill_honours_clip_rectangle, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/overlay-fill-honours-clip-rectangle",
        test_overlay_fill_honours_clip_rectangle, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/masked-fill-honours-clip-rectangle",
        test_masked_fill_honours_clip_rectangle, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/glyph-row-honours-clip-rectangle",
        test_glyph_row_honours_clip_rectangle, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/overlay-glyph-row-under-zero-colour-mask",
        test_overlay_glyph_row_under_zero_colour_mask, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-line-clip-follows-xywin-origin",
        test_re_line_clip_follows_xywin_origin, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/re-char-h-execute",
        test_re_char_h_execute, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/glyph-wide",
        test_public_fifo_glyph_wide, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/glyph-narrow",
        test_public_fifo_glyph_narrow, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/xor",
        test_public_fifo_xor, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/fill",
        test_public_fifo_fill, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/"
        "unsupported-execute-selectors",
        test_public_fifo_unsupported_execute_selectors, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/glyph-widths",
        test_public_fifo_glyph_widths, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/unsupported",
        test_public_fifo_unsupported, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/glyph-mask",
        test_public_fifo_glyph_mask, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/public-fifo/glyph-validity",
        test_public_fifo_glyph_validity, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/fifo-re-blockcopy-unsupported",
        test_fifo_re_blockcopy_unsupported, true);

    ret = g_test_run();
    if (qts) {
        qtest_quit(qts);
        qts = NULL;
    }

    return ret;
}
