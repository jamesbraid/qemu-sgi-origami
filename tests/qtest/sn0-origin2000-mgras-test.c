/*
 * SI Viz register-contract and protocol qtests.
 *
 * Unsupported behavior remains an approximation unless the public MGRAS
 * contracts specify it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include <sys/wait.h>
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define WIDGET_BASE(w)    (SPACE_IO + ((uint64_t)(w) << 24))
#define SI_WIDGET         0xf
#define MGRAS_BASE           WIDGET_BASE(SI_WIDGET)
#define MGRAS_REG(offset)    (MGRAS_BASE + (offset))

/*
 * The register cases share one machine: nothing they read depends on the
 * CPU count or the memory size, so neither is spelled out here. The
 * migration cases start their own pairs and keep the fuller machine,
 * because what they check crosses a whole machine's saved state.
 */
#define MACHINE_ARGS "-M origin2000 -device sgi-mgras,slot=io3"
#define MIGRATION_ARGS \
    "-M origin2000 -smp 8 -m 256M -device sgi-mgras,slot=io3"

/*
 * Started once in main() and reset before each register case. Booting a
 * machine per case cost about three seconds each and made this the
 * slowest qtest in the suite for no extra coverage.
 */
static QTestState *qts;


#define REG_IDENTIFICATION     MGRAS_REG(0x000004)
#define REG_WIDGET_CONTROL     MGRAS_REG(0x000024)
#define REG_INT_DEST_UPPER     MGRAS_REG(0x000034)
#define REG_INT_DEST_LOWER     MGRAS_REG(0x00003c)
#define REG_STARTUP_STATUS     MGRAS_REG(0x008000)
#define REG_BRINGUP_STEP       MGRAS_REG(0x011098)
#define REG_PREFIX             MGRAS_REG(0x011000)
#define REG_RESET_ARG          MGRAS_REG(0x011008)
#define REG_PENDING            MGRAS_REG(0x011020)
#define REG_PENDING_ACK        MGRAS_REG(0x011028)
#define REG_IRQ_MASK           MGRAS_REG(0x011040)
#define REG_HQ_STATUS          MGRAS_REG(0x020000)
#define REG_FIFO_STATUS        MGRAS_REG(0x020008)
#define REG_BUS_STATUS         MGRAS_REG(0x020100)
#define REG_DMA_BUSY           MGRAS_REG(0x020200)
#define REG_DIRECT_COPY_READY  MGRAS_REG(0x02c578)
#define REG_RESET_CONTROL      MGRAS_REG(0x072408)
#define REG_I2C_SCRATCH        MGRAS_REG(0x073c08)
#define REG_I2C_STROBE         MGRAS_REG(0x073c88)
#define REG_REPP_STATUS_1      MGRAS_REG(0x071c08)
#define REG_XMAP_INDEX         MGRAS_REG(0x071c88)
#define REG_REPP_STATUS_3      MGRAS_REG(0x071d00)
#define REG_REPP_STATUS_4      MGRAS_REG(0x071d08)
#define REG_SYNC_STATUS        MGRAS_REG(0x061038)
#define REG_REPP_SYNC          MGRAS_REG(0x071f80)
#define REG_DAC_LUT_INDEX      MGRAS_REG(0x071810)
#define REG_DAC_LUT_DATA       MGRAS_REG(0x071890)
#define REG_DAC_LUT_DATA_2     MGRAS_REG(0x071910)
#define REG_DAC_CONFIG         MGRAS_REG(0x071988)
#define REG_DAC_LUT_INDEX_LOW  MGRAS_REG(0x071811)
#define REG_HQ_UPLOAD           MGRAS_REG(0x050000)
#define REG_GE_DATA_A           MGRAS_REG(0x058048)
#define REG_GE_CTRL_A           MGRAS_REG(0x05804c)
#define REG_GE_DATA_B           MGRAS_REG(0x058040)
#define REG_GE_CTRL_B           MGRAS_REG(0x058044)
#define REG_GE_READ_1           MGRAS_REG(0x05822c)
#define REG_GE_READ_2           MGRAS_REG(0x058230)
#define REG_ENTRY               MGRAS_REG(0x130500)
#define REG_CONTEXT_COMMAND     MGRAS_REG(0x120400)
#define REG_VC3_INDEXDATA       MGRAS_REG(0x072038)
#define REG_VC3_INDEX           MGRAS_REG(0x072008)
#define REG_VC3_DATA            MGRAS_REG(0x0720b0)
#define REG_VC3_RAM             MGRAS_REG(0x072190)
#define REG_BOARD_INVENTORY     MGRAS_REG(0x072488)
#define REG_CMAP_WRITE_INDEX    MGRAS_REG(0x070c30)
#define REG_CMAP_DATA           MGRAS_REG(0x070d18)
#define REG_XMAP_MAIN_MODE      MGRAS_REG(0x071e00)
#define REG_XMAP_BUF_SELECT     MGRAS_REG(0x071d80)
#define REG_INT_DEST_UPPER_ALT  MGRAS_REG(0x000030)
#define REG_INT_DEST_LOWER_ALT  MGRAS_REG(0x000038)
#define REG_HQ_CONFIG_A         MGRAS_REG(0x011030)
#define REG_HQ_CONFIG_B         MGRAS_REG(0x011038)
#define REG_HQ_ENABLE           MGRAS_REG(0x011050)
#define REG_HQ_ENABLE_CLEAR     MGRAS_REG(0x011058)
#define REG_HQ_VECTOR_DEST_UPPER MGRAS_REG(0x011060)
#define REG_HQ_VECTOR_DEST_LOWER MGRAS_REG(0x011068)
#define REG_HQ_VECTORS           MGRAS_REG(0x011070)
#define REG_FIFO_THRESHOLD_0     MGRAS_REG(0x040000)
#define REG_FIFO_THRESHOLD_1     MGRAS_REG(0x040008)
#define REG_FIFO_THRESHOLD_LAST  MGRAS_REG(0x040030)
#define REG_HQ_FLAGS            MGRAS_REG(0x020010)
#define REG_HQ_FLAGS_CLEAR      MGRAS_REG(0x020018)
#define REG_SCHED_ARG_4         MGRAS_REG(0x040070)

#define MGRAS_ENTRY_WORD           0x000004fc
#define MGRAS_FINAL_TOKEN           0x0000f400
#define MGRAS_PENDING_FLAGS         0x00080000
#define MGRAS_PENDING_CONTEXT       0x10000000
#define MGRAS_PENDING_RETRACE       0x00000001
#define MGRAS_PENDING_ERROR         0x00000002
#define MGRAS_PENDING_LOWATER       0x00000004
#define MGRAS_HQ_ENABLE_LOWATER     0x10
#define MGRAS_CONTEXT_WORDS         63
#define MGRAS_PENDING_SWAP          0x00000400
#define REG_IRQ_ENABLE_CLEAR        MGRAS_REG(0x011048)
#define REG_QUERY_ANSWER            MGRAS_REG(0x028004)
#define MGRAS_SWAP_TOKEN            0x00003700
#define MGRAS_QUERY_TOKEN           0x0000a20c
#define MGRAS_HQ_FLAG_QUERY         0x00020000


#define MGRAS_FENCE_PUSH_1          0x00080b04
#define MGRAS_FENCE_PUSH_2          0x000000b1000e0400ULL
#define MGRAS_FENCE_TOKEN           0x000e0400
#define MGRAS_HQ_FLAG_FENCE         0x00010000

#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE(v)    ((v) & 1)

#define MGRAS_PART             0xc003
#define MGRAS_MANUFACTURER     0x02aa


#define MGRAS_COMMAND(reg, value) \
    ((((uint64_t)(0x60001 | ((reg) << 6))) << 34) | (uint32_t)(value))
#define MGRAS_COMMAND_EXEC(reg, value) \
    ((((uint64_t)(0x70001 | ((reg) << 6))) << 34) | (uint32_t)(value))


#define VC3_INDEXDATA(index, data16) \
    (((uint32_t)(index) << 24) | (((uint32_t)(data16) & 0xffff) << 8))

/*
 * The raster clip rectangle and its enable; see MGRAS_RSS_CLIP_X in
 * hw/misc/sgi-mgras-raster.c.
 */
#define REG_CLIP_X              0x147
#define REG_CLIP_Y              0x148
#define REG_CLIP_MODE           0x14f
#define MGRAS_CLIP_MODE_ON      0x11
#define REG_FILL_COLOR          0x140
#define REG_BLOCK_START         0x46
#define REG_BLOCK_END           0x47
#define REG_PACKEDCOLOR         0x05b
#define REG_RED                 0x05c
#define REG_CHAR_H              0x070
#define REG_CHAR_L              0x071
#define REG_BKGRD_RG            0x140
#define REG_BKGRD_BA            0x141
#define REG_PP1FILLMODE         0x161
#define REG_COLORMASKMSBS       0x162
#define REG_RE_POINT            0x040
#define REG_RE_LINE             0x041
#define REG_RE_BLOCKCOPY        0x045
#define REG_XYWIN               0x115

#define REG_IR_ALIAS            0x045
#define REG_ALPHA               0x05f
#define REG_COLORMASKLSBSA      0x163
#define REG_GLINE_XSTARTF       0x00c
#define REG_DRBPOINTERS         0x16d

/*
 * FILLMODE (OpenBSD impact(4) driver name) picks a CHAR_H/CHAR_L execute's
 * paint mode; see hw/misc/sgi-mgras-raster.c.
 */
#define REG_FILLMODE             0x110
#define MGRAS_FILLMODE_GLYPH_ROW    0x10400008
#define MGRAS_FILLMODE_BYTE_XFER    0x10c00000
#define MGRAS_FILLMODE_RECT_FILL_MASK  0x00500000
#define MGRAS_FILLMODE_RECT_FILL_VALUE 0x00100000

/*
 * Stippled lines: FILLMODE 0x20 gates each pixel on the 0x15a pattern and
 * 0x40 paints clear bits with BKGRD_RG/BKGRD_BA; see
 * MGRAS_RSS_LINE_STIPPLE in hw/misc/sgi-mgras-raster.c.
 */
#define REG_LINE_STIPPLE              0x15a
#define MGRAS_FILLMODE_STIPPLE        0x00000020
#define MGRAS_FILLMODE_STIPPLE_OPAQUE 0x00000060

/*
 * A rect fill's colour comes from these registers, not PACKEDCOLOR: CI8
 * masks FILLCOLOR_R to a CMAP index byte, RGB packs the low byte of each
 * of the three high nibbles; see mgras_draw_fill() in hw/misc/sgi-mgras-raster.c.
 */
#define REG_FILLCOLOR_R          0x176
#define REG_FILLCOLOR_G          0x177
#define REG_FILLCOLOR_B          0x178

/*
 * Byte-transfer mode's own packing register; see
 * hw/misc/sgi-mgras-raster.c's MGRAS_RSS_BYTE_XFER_PACKING comment.
 */
#define REG_BYTE_XFER_PACKING     0x159
#define MGRAS_BYTE_XFER_PACKING_BYTES 0x8000
#define MGRAS_BYTE_XFER_PACKING_PAIRS 0x0080


/* The full X11 GX raster-op encoding PP1FILLMODE bits 29:26 select. */
#define MGRAS_LOGIC_OP_CLEAR         0x0
#define MGRAS_LOGIC_OP_AND           0x1
#define MGRAS_LOGIC_OP_AND_REVERSE   0x2
#define MGRAS_LOGIC_OP_COPY          0x3
#define MGRAS_LOGIC_OP_AND_INVERTED  0x4
#define MGRAS_LOGIC_OP_NOOP          0x5
#define MGRAS_LOGIC_OP_XOR           0x6
#define MGRAS_LOGIC_OP_OR            0x7
#define MGRAS_LOGIC_OP_NOR           0x8
#define MGRAS_LOGIC_OP_EQUIV         0x9
#define MGRAS_LOGIC_OP_INVERT        0xa
#define MGRAS_LOGIC_OP_OR_REVERSE    0xb
#define MGRAS_LOGIC_OP_COPY_INVERTED 0xc
#define MGRAS_LOGIC_OP_OR_INVERTED   0xd
#define MGRAS_LOGIC_OP_NAND          0xe
#define MGRAS_LOGIC_OP_SET           0xf

/*
 * The X server's whole RGB pixel word, as written to PACKEDCOLOR or a
 * pair-packed CHAR_H/CHAR_L, has red in bits 7:0 (0x00BBGGRR); the model
 * stores 0x00RRGGBB. See mgras_rgb_from_pixel_word() in the raster model.
 * Tests write MGRAS_PIXEL_WORD(rgb) so their expected pixel stays rgb.
 */
#define MGRAS_PIXEL_WORD(rgb) \
    ((((rgb) & 0xffu) << 16) | ((rgb) & 0xff00u) | (((rgb) >> 16) & 0xffu))

/* PP1FILLMODE bit 13; see MGRAS_RSS_FILLCOLOR_R's comment in the raster model. */
#define MGRAS_PP1FILLMODE_RGB      0x2000

/* x-test-format values; see SGI_MGRAS_PIXEL_FORMAT_* in sgi-mgras.h. */
#define MGRAS_PIXEL_FORMAT_CI8     0
#define MGRAS_PIXEL_FORMAT_RGB     1
#define MGRAS_PIXEL_FORMAT_RGB12   2

/*
 * The IRIX 6.5.30 server's PP1FILLMODE for a 12-bit RGB window: 0x4004 for
 * its drawing and 0xc804 for the clear that opens it.
 */
#define MGRAS_PP1FILLMODE_RGB12        0x4004
#define MGRAS_PP1FILLMODE_RGB12_CLEAR  0xc804


#define MGRAS_STATE_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x18 | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))
#define MGRAS_EXEC_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x1c | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))


#define MGRAS_QOM_PATH "/machine/module0/io3"

static uint64_t mgras_qom_get_uint(QTestState *qts, const char *property)
{
    QDict *resp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", MGRAS_QOM_PATH, property);
    uint64_t value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) s = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, s->str);
    }
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return value;
}

/*
 * Open a register case. The model's reset restores every field this file
 * reads, and asserting the settled bring-up status and a clear sticky
 * error here is what proves it: a case cannot silently inherit the
 * register face the previous case left behind.
 */
static void mgras_reset(void)
{
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static uint64_t mgras_read_width(QTestState *qts, uint64_t addr,
                              unsigned int width)
{
    switch (width) {
    case 1:
        return qtest_readb(qts, addr);
    case 2:
        return qtest_readw(qts, addr);
    case 4:
        return qtest_readl(qts, addr);
    case 8:
        return qtest_readq(qts, addr);
    default:
        g_assert_not_reached();
    }
}

static void mgras_write_width(QTestState *qts, uint64_t addr,
                           unsigned int width, uint64_t value)
{
    switch (width) {
    case 1:
        qtest_writeb(qts, addr, value);
        return;
    case 2:
        qtest_writew(qts, addr, value);
        return;
    case 4:
        qtest_writel(qts, addr, value);
        return;
    case 8:
        qtest_writeq(qts, addr, value);
        return;
    default:
        g_assert_not_reached();
    }
}

static void mgras_assert_width_error(QTestState *qts, uint64_t addr)
{
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 1);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error-addr"), ==,
                    addr - MGRAS_BASE);
}

static uint32_t mgras_test_pixel(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-pixel");
}


static uint32_t mgras_test_display_pixel(QTestState *qts, uint32_t x,
                                         uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-display-pixel");
}

static uint32_t mgras_test_wid(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-wid");
}

static uint32_t mgras_test_overlay(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-overlay");
}

static uint32_t mgras_test_format(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-format");
}

static uint32_t mgras_screendump_pixel(QTestState *qts, uint32_t x, uint32_t y)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    char *pixels;
    gsize length;
    int fd;

    fd = g_file_open_tmp("sgi-mgras-surface-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    {
        QDict *response = qtest_qmp(qts,
            "{'execute': 'screendump', 'arguments': {'filename': %s}}",
            path);
        if (!qdict_haskey(response, "return")) {
            g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
            g_error("screendump failed: %s", text->str);
        }
        qobject_unref(response);
    }
    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(contents, "P6\n1280 1024\n255\n"));
    pixels = g_strstr_len(contents, length, "\n255\n");
    g_assert_nonnull(pixels);
    pixels += 5;
    g_assert_cmpuint(x, <, 1280);
    g_assert_cmpuint(y, <, 1024);
    g_assert_cmpuint((gsize)(pixels - contents) + 1280 * 1024 * 3, <=,
                     length);
    pixels += (y * 1280 + x) * 3;
    unlink(path);
    return ((uint8_t)pixels[0] << 16) | ((uint8_t)pixels[1] << 8) |
           (uint8_t)pixels[2];
}


static void mgras_enter_context(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, REG_STARTUP_STATUS) & 0x1eff, ==, 0);

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                     MGRAS_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==, 0);

    /*
     * No closing word exists at 0x120400: the image completes as soon as
     * the 63rd word lands.
     */
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                     MGRAS_PENDING_CONTEXT);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
}

static void mgras_write_cmap_position(QTestState *qts, uint16_t selector,
                                   unsigned int position, uint32_t color)
{
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, selector);
    for (unsigned int i = 0; i < position; i++) {
        qtest_writel(qts, REG_CMAP_DATA, 0);
    }
    qtest_writel(qts, REG_CMAP_DATA, color);
}

/* Address one entry directly: bits 15:8 are the entry, bits 4:0 the map. */
static void mgras_write_cmap_entry(QTestState *qts, unsigned int map,
                                unsigned int entry, uint32_t color)
{
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (entry << 8) | map);
    qtest_writel(qts, REG_CMAP_DATA, color);
}

/*
 * Replay a captured gamma-table load: a halfword reset to REG_DAC_LUT_INDEX,
 * then 256 rows of three halfword writes to REG_DAC_LUT_DATA (red, green,
 * blue), matching the 768-write bursts a desktop session's login sends. Each
 * write's high byte is the stored gamma output; the low byte is unused.
 */
static void mgras_load_dac_gamma(QTestState *qts, const uint8_t ramp[3][256])
{
    qtest_writew(qts, REG_DAC_LUT_INDEX, 0);
    for (unsigned int row = 0; row < 256; row++) {
        for (unsigned int channel = 0; channel < 3; channel++) {
            qtest_writew(qts, REG_DAC_LUT_DATA,
                         (uint16_t)ramp[channel][row] << 8);
        }
    }
}

/*
 * Give a rectangle a window ID the way the X server does: through the VC3
 * main display-ID table (register 4), a frame table of one SRAM word per
 * scanline pointing at a run list of (column << 5 | WID) entries ended by a
 * column past the screen. Each tagged row is read back from SRAM, the
 * rectangle laid over it, and a fresh run list appended, so earlier tags on
 * the row survive, including ones a migration carried over. An untagged
 * row's frame entry is still its post-reset zero, which points at SRAM word
 * 0 like any other row would: seed a run list there the first time it is
 * read, rather than treating a zero frame entry as a special empty case.
 */
#define MGRAS_DID_FRAME_BASE 0x0500
#define MGRAS_DID_NEXT_ADDR  (MGRAS_DID_FRAME_BASE + 1024)
#define MGRAS_DID_RUNS_BASE  0x2000

static uint16_t mgras_vc3_sram_read(QTestState *qts, uint16_t addr)
{
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, addr));
    return qtest_readw(qts, REG_VC3_RAM);
}

static void mgras_did_read_row(QTestState *qts, uint32_t y, uint8_t *did)
{
    uint16_t ptr = mgras_vc3_sram_read(qts, MGRAS_DID_FRAME_BASE + y);
    uint16_t run;
    uint8_t current;

    memset(did, 0, 1280);
    if (!ptr) {
        /*
         * A frame entry of zero is not a special case: it points at SRAM
         * word 0 like any other row would. Seed a one-entry run list there
         * (WID 0, then the screen-edge terminator) so the walk below
         * follows an ordinary pointer instead of relying on word 0
         * happening to still hold its post-reset zero.
         */
        qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0));
        qtest_writew(qts, REG_VC3_RAM, 0);
        qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 1));
        qtest_writew(qts, REG_VC3_RAM, 0xffe0);
    }
    run = mgras_vc3_sram_read(qts, ptr);
    current = run & 0x1f;
    for (uint32_t x = 0; x < 1280;) {
        run = mgras_vc3_sram_read(qts, ++ptr);
        g_assert_cmpuint(run >> 5, >, x);
        for (; x < MIN(run >> 5, 1280u); x++) {
            did[x] = current;
        }
        current = run & 0x1f;
    }
}

static void mgras_tag_wid(QTestState *qts, uint32_t x0, uint32_t y0,
                       uint32_t x1, uint32_t y1, uint8_t wid)
{
    uint16_t next = mgras_vc3_sram_read(qts, MGRAS_DID_NEXT_ADDR);

    if (!next) {
        next = MGRAS_DID_RUNS_BASE;
    }
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(0x04, MGRAS_DID_FRAME_BASE));
    for (uint32_t y = y0; y <= y1; y++) {
        uint8_t did[1280];
        uint16_t list = next;

        mgras_did_read_row(qts, y, did);
        memset(&did[x0], wid, x1 - x0 + 1);
        qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, list));
        qtest_writew(qts, REG_VC3_RAM, did[0]);
        next++;
        for (uint32_t x = 1; x < 1280; x++) {
            if (did[x] != did[x - 1]) {
                qtest_writew(qts, REG_VC3_RAM, (x << 5) | did[x]);
                next++;
            }
        }
        qtest_writew(qts, REG_VC3_RAM, 0xffe0);
        next++;
        qtest_writel(qts, REG_VC3_INDEXDATA,
                     VC3_INDEXDATA(0x07, MGRAS_DID_FRAME_BASE + y));
        qtest_writew(qts, REG_VC3_RAM, list);
    }
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(0x07, MGRAS_DID_NEXT_ADDR));
    qtest_writew(qts, REG_VC3_RAM, next);
}

static void test_identity_and_aperture(void)
{
    uint32_t id;

    mgras_reset();

    id = qtest_readl(qts, REG_IDENTIFICATION);
    g_assert_cmphex(ID_PART(id), ==, MGRAS_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, MGRAS_MANUFACTURER);
    g_assert_cmphex(ID_ALWAYS_ONE(id), ==, 1);


    g_assert_cmphex(qtest_readl(qts, REG_STARTUP_STATUS) & 0x1eff, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_DMA_BUSY) & 0x1f, ==, 0);
    /* Bits 7:4 are the default SI board's RE4 revision, 2. */
    g_assert_cmphex(qtest_readl(qts, REG_DIRECT_COPY_READY), ==, 0x120);
}

/*
 * Inferred (see the read/write-side comments above these two cases in
 * mgras_read_internal()/mgras_write()): the first halves of the
 * 64-bit-aligned widget-register slots ignore writes rather than raising
 * a guest error a real access hits. 0x000020 reads zero; 0x000000 reads
 * only the HQ revision in bits 31:28 (2 on the default SI board). Reading
 * REG_IDENTIFICATION back unchanged after writing both proves the write
 * side does not alias into the neighboring real register.
 */
static void test_widget_reserved_halves_raz_wi(void)
{
    uint32_t identification;

    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000000)), ==, 0x20000000);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000020)), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    identification = qtest_readl(qts, REG_IDENTIFICATION);
    qtest_writel(qts, MGRAS_REG(0x000000), 0xffffffff);
    qtest_writel(qts, MGRAS_REG(0x000020), 0xffffffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000000)), ==, 0x20000000);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000020)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_IDENTIFICATION), ==, identification);
}


static void test_widget_control_is_plain_storage(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x0000300f);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x0000300f);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x00003000);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x00003000);
}


static void test_generic_bringup_step(void)
{
    mgras_reset();

    qtest_writel(qts, REG_BRINGUP_STEP, (520u << 10) | (65u << 2));
    g_assert_cmphex(qtest_readl(qts, REG_BRINGUP_STEP) & 0x2, ==, 0x2);
}


static void test_bus_status_reads_settled(void)
{
    mgras_reset();

    g_assert_cmpuint(((qtest_readl(qts, REG_BUS_STATUS) & 0x1f) + 1) >> 1, <, 8);
}


static void test_reset_registers_are_plain_storage(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0);
    qtest_writel(qts, REG_RESET_ARG, 0x765b900f);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0x765b900f);


    qtest_writel(qts, REG_RESET_CONTROL, 0x40000000);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0);
}


static void test_i2c_probe_loopback(void)
{
    mgras_reset();

    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    qtest_writeb(qts, REG_I2C_SCRATCH, 0x55);
    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0x55);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeb(qts, REG_I2C_SCRATCH, 0xaa);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0xaa);
}

/*
 * REG_I2C_STROBE's byte write above was already accepted as a no-op; this
 * proves the read side now mirrors it, answering the approximated
 * idle/ack value instead of raising a guest error.
 */
static void test_i2c_strobe_byte_read(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readb(qts, REG_I2C_STROBE), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}


static void test_repp_sync_handshake(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_REPP_SYNC) & 0x00000f0f, ==,
                     0x00000101);


    qtest_writeb(qts, REG_XMAP_INDEX, 0x1c);
    g_assert_cmphex(qtest_readb(qts, REG_XMAP_INDEX), ==, 0x1c);

    /*
     * 0x071c08 is PP1SELECT; 1 reaches every PP1, as the kernel leaves it
     * after its per-PP1 loops.
     */
    qtest_writeb(qts, REG_REPP_STATUS_1, 1);
    qtest_writeb(qts, REG_XMAP_INDEX, 0x1c);
    qtest_writel(qts, REG_REPP_STATUS_3, 0x653);
    qtest_writel(qts, REG_REPP_STATUS_4, 0x10214);
    qtest_writel(qts, REG_SYNC_STATUS, 1);
    qtest_writel(qts, REG_REPP_SYNC, 0x50214);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_1), ==, 1);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_3), ==, 0x653);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_4), ==, 0x10214);
    g_assert_cmphex(qtest_readl(qts, REG_SYNC_STATUS), ==, 1);

    g_assert_cmphex(qtest_readb(qts, REG_XMAP_INDEX), ==, 0x1c);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_SYNC) & 0x00000f0f, ==,
                     0x00000101);
}

/*
 * PP1SELECT (0x071c08, REG_REPP_STATUS_1) and CONFIGB (0x071d08,
 * REG_REPP_STATUS_4) are the two REPP status ports gxemul's
 * impactsr-bsd.h and OpenBSD's impactreg.h declare byte-wide
 * (IMPACTSR_REG8). test_repp_sync_handshake above already covers their
 * word-width storage; this proves the newly accepted byte width is the
 * same plain storage, at no width other than the ones now allowed.
 */
static void test_repp_status_byte_writes(void)
{
    mgras_reset();

    /* RE 0's second PP1, which CONFIGB below then reaches. */
    qtest_writeb(qts, REG_REPP_STATUS_1, 0x02);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_1), ==, 0x02);

    qtest_writeb(qts, REG_REPP_STATUS_4, 0x21);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_4), ==, 0x21);

    mgras_reset();
    mgras_write_width(qts, REG_REPP_STATUS_1, 2, 0xff);
    mgras_assert_width_error(qts, REG_REPP_STATUS_1);
}

/*
 * XMAP_BUF_SELECT is a word register indexed by REG_XMAP_INDEX the same way as
 * REG_XMAP_MAIN_MODE. Other widths raise the width exception.
 */
static void test_xmap_buf_select_write(void)
{
    mgras_reset();

    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 0xffffffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_writeb(qts, REG_XMAP_INDEX, 4);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 0x0000ffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    mgras_reset();
    mgras_write_width(qts, REG_XMAP_BUF_SELECT, 1, 0xff);
    mgras_assert_width_error(qts, REG_XMAP_BUF_SELECT);

    mgras_reset();
    mgras_write_width(qts, REG_XMAP_BUF_SELECT, 8, 0xff);
    mgras_assert_width_error(qts, REG_XMAP_BUF_SELECT);

    mgras_reset();
    mgras_read_width(qts, REG_XMAP_BUF_SELECT, 1);
    mgras_assert_width_error(qts, REG_XMAP_BUF_SELECT);
}

/*
 * SGI's Octane IDE XMAP test (mg_xmapdcbreg, ide.IP30 6.5.30 at
 * 0xa80000002086907c) writes each of eight patterns to XMAP_BUF_SELECT
 * and reads it back: all 32 bits under index byte 0, which it calls the
 * main buffer select, and the low 8 bits under index byte 4, the overlay
 * buffer select. A mismatch fails the board.
 */
static void test_xmap_buf_select_reads_back(void)
{
    static const uint32_t patterns[] = {
        0x00000001, 0x00000002, 0x00000004, 0x00000008,
        0x10000000, 0x20000000, 0x40000000, 0x80000000,
    };

    mgras_reset();
    for (int i = 0; i < ARRAY_SIZE(patterns); i++) {
        qtest_writeb(qts, REG_XMAP_INDEX, 0);
        qtest_writel(qts, REG_XMAP_BUF_SELECT, patterns[i]);
        qtest_writeb(qts, REG_XMAP_INDEX, 0);
        g_assert_cmphex(qtest_readl(qts, REG_XMAP_BUF_SELECT), ==,
                        patterns[i]);

        qtest_writeb(qts, REG_XMAP_INDEX, 4);
        qtest_writel(qts, REG_XMAP_BUF_SELECT, patterns[i] & 0xff);
        qtest_writeb(qts, REG_XMAP_INDEX, 4);
        g_assert_cmphex(qtest_readl(qts, REG_XMAP_BUF_SELECT) & 0xff, ==,
                        patterns[i] & 0xff);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The same IDE test reads 0x071d00 under two index bytes after writing
 * 0xff000000 there under index byte 1. Index 4 is the XMAP revision and
 * passes only on 0 or 1; index 8 is the FIFO capacity, whose low nibble
 * must be 1. The IRIX bring-up path reads the revision the same way
 * (index 4, low three bits). Neither read may echo the earlier write.
 */
static void test_xmap_revision_and_fifo_capacity(void)
{
    uint32_t rev, cap;

    mgras_reset();
    qtest_writeb(qts, REG_XMAP_INDEX, 1);
    qtest_writel(qts, REG_REPP_STATUS_3, 0xff000000);

    qtest_writeb(qts, REG_XMAP_INDEX, 4);
    rev = qtest_readl(qts, REG_REPP_STATUS_3);
    g_assert_true(rev == 0 || rev == 1);

    qtest_writeb(qts, REG_XMAP_INDEX, 8);
    cap = qtest_readl(qts, REG_REPP_STATUS_3);
    g_assert_cmphex(cap & 0xf, ==, 1);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * PP1SELECT (0x071c08) picks which PP1 the XMAP registers reach. The IRIX
 * 6.5.30 kernel writes ((re << 1) | pp) << 1 there before its per-PP1
 * sequences (MgrasSyncREPP, mgras_update_pp1ccv),
 * so 0 and 2 name the two PP1s of RE 0 and 4 and 6 those of RE 1, and it
 * writes 1 before configuring every PP1 at once (MgrasInitXMAP,
 * MgrasSyncPPRDRAMs) and again after each per-PP1 loop
 * (docs/datasheets/mgras-dual-re.md). A two-RE board has four PP1s, each
 * with its own XMAP register file; 0x071f00 and 0x071f80 are the
 * exception, kept by no PP1 (test_pp1_select_leaves_sync_registers_shared).
 */
#define REG_PP1_SELECT          REG_REPP_STATUS_1
#define PP1_SELECT_ALL          1
#define PP1_SELECT(re, pp)      ((((re) << 1) | (pp)) << 1)

static void mgras_select_xmap(QTestState *s, uint8_t select, uint8_t index)
{
    qtest_writeb(s, REG_PP1_SELECT, select);
    qtest_writeb(s, REG_XMAP_INDEX, index);
}

static void test_pp1_select_routes_xmap_on_two_re_board(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-mgras.board=ssi");

    mgras_select_xmap(s, PP1_SELECT_ALL, 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x111);
    qtest_writel(s, REG_XMAP_BUF_SELECT, 0x5);
    qtest_writel(s, REG_REPP_STATUS_3, 0x0018073f);
    for (int re = 0; re < 2; re++) {
        for (int pp = 0; pp < 2; pp++) {
            mgras_select_xmap(s, PP1_SELECT(re, pp), 0);
            g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==, 0x111);
            g_assert_cmphex(qtest_readl(s, REG_XMAP_BUF_SELECT), ==, 0x5);
            g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3), ==,
                            0x0018073f);
        }
    }

    /* One PP1 of RE 1 alone: the other three keep what they had. */
    mgras_select_xmap(s, PP1_SELECT(1, 1), 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x222);
    qtest_writel(s, REG_REPP_STATUS_3, 0x0008073f);
    for (int re = 0; re < 2; re++) {
        for (int pp = 0; pp < 2; pp++) {
            bool picked = re == 1 && pp == 1;

            mgras_select_xmap(s, PP1_SELECT(re, pp), 0);
            g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==,
                            picked ? 0x222 : 0x111);
            g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3), ==,
                            picked ? 0x0008073f : 0x0018073f);
        }
    }

    /* The index register belongs to each PP1 as well. */
    mgras_select_xmap(s, PP1_SELECT(0, 1), 8);
    qtest_writeb(s, REG_PP1_SELECT, PP1_SELECT(0, 0));
    g_assert_cmphex(qtest_readb(s, REG_XMAP_INDEX), ==, 0);
    qtest_writeb(s, REG_PP1_SELECT, PP1_SELECT(0, 1));
    g_assert_cmphex(qtest_readb(s, REG_XMAP_INDEX), ==, 8);

    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * A one-RE board has no PP1s behind 4 and 6: a write there changes nothing
 * the board has, and the two PP1 it does have still take writes to all.
 * What real hardware returns for a read of an absent PP1 is unknown; the
 * model answers 0. Nothing here is a guest-visible board error.
 */
static void test_pp1_select_absent_pp1_on_one_re_board(void)
{
    mgras_reset();

    mgras_select_xmap(qts, PP1_SELECT_ALL, 0);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x111);
    mgras_select_xmap(qts, PP1_SELECT(1, 0), 0);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x333);
    g_assert_cmphex(qtest_readl(qts, REG_XMAP_MAIN_MODE), ==, 0);

    mgras_select_xmap(qts, PP1_SELECT(0, 0), 0);
    g_assert_cmphex(qtest_readl(qts, REG_XMAP_MAIN_MODE), ==, 0x111);
    mgras_select_xmap(qts, PP1_SELECT(0, 1), 0);
    g_assert_cmphex(qtest_readl(qts, REG_XMAP_MAIN_MODE), ==, 0x111);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    mgras_reset();
}

/*
 * Scanout reads the first PP1's mode tables. The kernel and X server give
 * every PP1 the same tables, so which one scanout reads is invisible to
 * them. Which PP1 drives which pixels is unknown, so a mode written to one
 * other PP1 alone does not change the screen.
 */
static void test_pp1_scanout_follows_first_pp1(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-mgras.board=ssi");

    /* Map 29 is the one reset's mode 0x7a4 names; 0x384 names map 28. */
    mgras_enter_context(s);
    qtest_writel(s, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(s, 29, 0x40, 0x11223300);
    mgras_write_cmap_entry(s, 28, 0x40, 0x44556600);
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(s, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_display_pixel(s, 5, 5), ==, 0x00112233);

    mgras_select_xmap(s, PP1_SELECT(1, 0), 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x384);
    g_assert_cmphex(mgras_test_display_pixel(s, 5, 5), ==, 0x00112233);

    mgras_select_xmap(s, PP1_SELECT(0, 0), 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x384);
    g_assert_cmphex(mgras_test_display_pixel(s, 5, 5), ==, 0x00445566);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

#define REG_XMAP_SYNC_0         MGRAS_REG(0x071f00)
#define REG_XMAP_OVERLAY_MODE   MGRAS_REG(0x071e80)
#define MGRAS_TWO_RE_ARGS       " -global sgi-mgras.board=ssi"

/* The PP1SELECT value naming PP1 n: RE n / 2, its PP1 n % 2. */
static uint8_t mgras_pp1_select_one(unsigned int n)
{
    return PP1_SELECT(n / 2, n % 2);
}

/*
 * Give each of a two-RE board's four PP1s different XMAP contents: under
 * index 0, main mode 0x100 + n, buffer select 0x10 + n, overlay mode
 * 0x20 + n, 0x071d00 0x1000 + n and 0x071d08 0x2000 + n, and then index
 * byte 4 * (n + 1). No broadcast write follows, so the four stay apart.
 */
static void mgras_give_pp1s_distinct_xmaps(QTestState *s)
{
    for (unsigned int n = 0; n < 4; n++) {
        mgras_select_xmap(s, mgras_pp1_select_one(n), 0);
        qtest_writel(s, REG_XMAP_MAIN_MODE, 0x100 + n);
        qtest_writel(s, REG_XMAP_BUF_SELECT, 0x10 + n);
        qtest_writel(s, REG_XMAP_OVERLAY_MODE, 0x20 + n);
        qtest_writel(s, REG_REPP_STATUS_3, 0x1000 + n);
        qtest_writel(s, REG_REPP_STATUS_4, 0x2000 + n);
        qtest_writeb(s, REG_XMAP_INDEX, 4 * (n + 1));
    }
}

/* Read PP1 n's index byte, then its registers under index 0. */
static void mgras_assert_pp1_xmap(QTestState *s, uint8_t select,
                                  unsigned int n)
{
    qtest_writeb(s, REG_PP1_SELECT, select);
    g_assert_cmphex(qtest_readb(s, REG_XMAP_INDEX), ==, 4 * (n + 1));
    qtest_writeb(s, REG_XMAP_INDEX, 0);
    g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==, 0x100 + n);
    g_assert_cmphex(qtest_readl(s, REG_XMAP_BUF_SELECT), ==, 0x10 + n);
    g_assert_cmphex(qtest_readl(s, REG_XMAP_OVERLAY_MODE), ==, 0x20 + n);
    g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3), ==, 0x1000 + n);
    g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_4), ==, 0x2000 + n);
    qtest_writeb(s, REG_XMAP_INDEX, 4 * (n + 1));
}

/*
 * With every PP1 selected, a read answers from PP1 0. That is an
 * approximation: what hardware returns then is unknown
 * (docs/datasheets/mgras-dual-re.md). The four PP1s differ first, so the
 * answer shows which one gave it.
 */
static void test_pp1_select_all_reads_first_pp1(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    mgras_give_pp1s_distinct_xmaps(s);
    mgras_assert_pp1_xmap(s, PP1_SELECT_ALL, 0);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * PP1SELECT bits 7:3 are zero in every value IRIX writes, and the model
 * ignores them: with all five set, reads and writes reach the PP1 bits
 * 2:1 name, or every PP1 when bit 0 is set. What hardware does with them
 * is unknown.
 */
static void test_pp1_select_ignores_bits_7_to_3(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    mgras_give_pp1s_distinct_xmaps(s);
    for (unsigned int n = 0; n < 4; n++) {
        mgras_assert_pp1_xmap(s, 0xf8 | mgras_pp1_select_one(n), n);
    }

    mgras_select_xmap(s, 0xf8 | PP1_SELECT(1, 0), 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x999);
    for (unsigned int n = 0; n < 4; n++) {
        mgras_select_xmap(s, mgras_pp1_select_one(n), 0);
        g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==,
                        n == 2 ? 0x999 : 0x100 + n);
    }

    mgras_select_xmap(s, 0xf8 | PP1_SELECT_ALL, 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x777);
    for (unsigned int n = 0; n < 4; n++) {
        mgras_select_xmap(s, mgras_pp1_select_one(n), 0);
        g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==, 0x777);
    }
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * 0x071f00 and 0x071f80 sit among the XMAP registers IRIX reaches through
 * PP1SELECT, but the model keeps no copy of them for any PP1, an
 * approximation the datasheet's model contract names: under every
 * selection they read 1 and 0x101, and a write under any selection is
 * dropped.
 */
static void test_pp1_select_leaves_sync_registers_shared(void)
{
    static const uint8_t selects[] = {
        PP1_SELECT(0, 0), PP1_SELECT(0, 1), PP1_SELECT(1, 0),
        PP1_SELECT(1, 1), PP1_SELECT_ALL,
    };
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    for (size_t i = 0; i < ARRAY_SIZE(selects); i++) {
        mgras_select_xmap(s, selects[i], 0);
        qtest_writel(s, REG_XMAP_SYNC_0, 0x1230 + i);
        qtest_writel(s, REG_REPP_SYNC, 0x50210 + i);
        mgras_select_xmap(s, selects[i], 0x1c);
        qtest_writel(s, REG_REPP_SYNC, 0x650 + i);
    }
    for (size_t i = 0; i < ARRAY_SIZE(selects); i++) {
        mgras_select_xmap(s, selects[i], 0);
        g_assert_cmphex(qtest_readl(s, REG_XMAP_SYNC_0), ==, 1);
        g_assert_cmphex(qtest_readl(s, REG_REPP_SYNC), ==, 0x101);
        mgras_select_xmap(s, selects[i], 0x1c);
        g_assert_cmphex(qtest_readl(s, REG_REPP_SYNC), ==, 0x101);
    }
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * Both REs of a two-RE board draw into one set of planes in the model.
 * The OCTANE Technical Report has the second RE take alternate spans, so
 * on hardware adjacent rows may belong to different REs; no software path
 * IRIX uses can tell, and the model does not split them
 * (docs/datasheets/mgras-dual-re.md). Two adjacent rows filled with
 * different colours both read back from the planes scanout uses. A model
 * that split the REs' pixels would have to change this deliberately.
 */
static void test_two_re_board_draws_one_set_of_planes(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    mgras_enter_context(s);
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    for (unsigned int y = 20; y < 24; y++) {
        qtest_writeq(s, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_FILLCOLOR_R, 0x30 + y));
        qtest_writeq(s, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | y));
        qtest_writeq(s, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | y));
    }
    for (unsigned int y = 20; y < 24; y++) {
        g_assert_cmphex(mgras_test_pixel(s, 10, y), ==, 0x30 + y);
        g_assert_cmphex(mgras_test_pixel(s, 13, y), ==, 0x30 + y);
    }
    g_assert_cmphex(mgras_test_pixel(s, 10, 19), ==, 0);
    g_assert_cmphex(mgras_test_pixel(s, 10, 24), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

typedef struct SIVisualRegisterWidth {
    uint64_t addr;
    unsigned int width;
} SIVisualRegisterWidth;

static const SIVisualRegisterWidth mgras_visual_register_widths[] = {
    { REG_XMAP_INDEX, 1 },
    { REG_XMAP_MAIN_MODE, 4 },
    { REG_CMAP_WRITE_INDEX, 2 },
    { REG_CMAP_DATA, 4 },
};

static const unsigned int mgras_access_widths[] = { 1, 2, 4, 8 };

static void test_visual_register_read_widths(void)
{
    for (int reg = 0; reg < ARRAY_SIZE(mgras_visual_register_widths); reg++) {
        for (int width = 0; width < ARRAY_SIZE(mgras_access_widths); width++) {
            if (mgras_access_widths[width] ==
                mgras_visual_register_widths[reg].width) {
                continue;
            }
            mgras_reset();
            mgras_read_width(qts, mgras_visual_register_widths[reg].addr,
                          mgras_access_widths[width]);
            mgras_assert_width_error(qts,
                                     mgras_visual_register_widths[reg].addr);
        }
    }
}

static void test_visual_register_write_widths(void)
{
    for (int reg = 0; reg < ARRAY_SIZE(mgras_visual_register_widths); reg++) {
        for (int width = 0; width < ARRAY_SIZE(mgras_access_widths); width++) {
            if (mgras_access_widths[width] ==
                mgras_visual_register_widths[reg].width) {
                continue;
            }
            mgras_reset();
            mgras_write_width(qts, mgras_visual_register_widths[reg].addr,
                           mgras_access_widths[width], UINT64_MAX);
            mgras_assert_width_error(qts,
                                     mgras_visual_register_widths[reg].addr);
        }
    }
}


static void test_hq_status_bringup_bit(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);
}


static void test_hq_status_hiwater_poll_bit(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x10, ==, 0x10);
}


static void test_hq_status_independent_of_sticky_error(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);


    qtest_writel(qts, MGRAS_REG(0x090000), 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 1);


    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);
}


/*
 * A captured desktop session brackets each gamma-table load with writes to
 * these three registers at the widths it actually uses (byte, confirmed
 * against the capture); their role is otherwise unconfirmed, so the model
 * keeps them as opaque flat storage. REG_DAC_LUT_INDEX takes the halfword
 * width the capture uses and resets the load position that
 * test_dac_gamma_identity_ramp_leaves_colors_unchanged and
 * test_dac_gamma_curve_applies_to_scanout exercise through the data port,
 * since the position itself has no readable register.
 */
static void test_dac_lut_load(void)
{
    mgras_reset();

    qtest_writeb(qts, REG_DAC_CONFIG, 0x1c);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_CONFIG), ==, 0x1c);

    /*
     * REG_DAC_LUT_INDEX_LOW sits at an odd address: no read width is both
     * aligned to it and within the read-side whitelist, so unlike the
     * other bracketing registers its write cannot be confirmed by a
     * readback. A captured session never reads it either.
     */
    qtest_writeb(qts, REG_DAC_LUT_INDEX_LOW, 0xff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_writeb(qts, REG_DAC_LUT_DATA_2, 0x01);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_DATA_2), ==, 0x01);

    qtest_writew(qts, REG_DAC_LUT_INDEX, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_INDEX), ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}

/*
 * SGI's Octane IDE DAC reset (mg_dacreset, ide.IP30 6.5.30 at
 * 0xa800000020854920) addresses DAC registers through REG_DAC_LUT_INDEX,
 * a halfword whose high byte is the register number, and moves their
 * data through REG_DAC_LUT_DATA_2 a byte at a time. It fails the board
 * unless the address reads back, register 3 (the DAC ID) is 0x79 with
 * register 0xb (its revision) at 1 -- or 0x76 with 0, which it warns
 * about -- and each register it programs reads back under mask 0xff.
 * A captured IRIX desktop session programs the same registers, in the
 * same order, before each gamma-table load (0x0600 takes 0xec there).
 */
static void test_dac_register_file_matches_ide_reset(void)
{
    static const struct {
        uint16_t addr;
        uint8_t value;
    } regs[] = {
        { 0x0400, 0xff }, { 0x0500, 0x89 }, { 0x0600, 0xe4 },
        { 0x0700, 0xc0 }, { 0x0800, 0x06 }, { 0x0402, 0x01 },
    };
    uint8_t id, rev;

    mgras_reset();
    qtest_writeb(qts, REG_DAC_CONFIG, 3);
    qtest_writeb(qts, REG_DAC_CONFIG, 2);
    qtest_writeb(qts, REG_DAC_CONFIG, 3);
    /* Register number 5 through the byte port: command register 1. */
    qtest_writeb(qts, REG_DAC_LUT_INDEX_LOW, 5);
    qtest_writeb(qts, REG_DAC_LUT_DATA_2, 0x89);
    qtest_writew(qts, REG_DAC_LUT_INDEX, 0x0500);
    g_assert_cmphex(qtest_readb(qts, REG_DAC_LUT_DATA_2), ==, 0x89);
    qtest_writew(qts, REG_DAC_LUT_INDEX, 0x0d00);
    qtest_writeb(qts, REG_DAC_LUT_DATA_2, 0x40);

    qtest_writew(qts, REG_DAC_LUT_INDEX, 0);
    g_assert_cmphex((qtest_readw(qts, REG_DAC_LUT_INDEX) >> 8) & 0xff, ==, 0);

    qtest_writew(qts, REG_DAC_LUT_INDEX, 0x0300);
    id = qtest_readb(qts, REG_DAC_LUT_DATA_2);
    qtest_writew(qts, REG_DAC_LUT_INDEX, 0x0b00);
    rev = qtest_readb(qts, REG_DAC_LUT_DATA_2);
    g_assert_true((id == 0x79 && rev == 1) || (id == 0x76 && rev == 0));

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        qtest_writew(qts, REG_DAC_LUT_INDEX, regs[i].addr);
        qtest_writeb(qts, REG_DAC_LUT_DATA_2, regs[i].value);
    }
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        qtest_writew(qts, REG_DAC_LUT_INDEX, regs[i].addr);
        g_assert_cmphex(qtest_readb(qts, REG_DAC_LUT_DATA_2), ==,
                        regs[i].value);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * SGI's Octane IDE DAC address test (mg_dacaddrreg, ide.IP30 6.5.30 at
 * 0xa800000020853c2c) writes a byte to 0x071811 and requires it back in
 * bits 15:8 of the halfword at 0x071810, the register number. It then
 * writes the halfword with the pattern in its high byte and checks the
 * same byte. The halfword's low byte is never compared.
 */
static void test_dac_address_byte_port(void)
{
    static const uint8_t patterns[] = {
        0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xff,
    };

    mgras_reset();
    for (int i = 0; i < ARRAY_SIZE(patterns); i++) {
        qtest_writeb(qts, REG_DAC_LUT_INDEX_LOW, patterns[i]);
        g_assert_cmphex((qtest_readw(qts, REG_DAC_LUT_INDEX) >> 8) & 0xff, ==,
                        patterns[i]);
    }
    for (int i = 0; i < ARRAY_SIZE(patterns); i++) {
        qtest_writew(qts, REG_DAC_LUT_INDEX, (uint16_t)patterns[i] << 8);
        g_assert_cmphex((qtest_readw(qts, REG_DAC_LUT_INDEX) >> 8) & 0xff, ==,
                        patterns[i]);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * SGI's Octane IDE VC3 register test (mg_vc3internalreg, ide.IP30 6.5.30
 * at 0xa80000002083da2c) stops video timing, then writes walking-one,
 * walking-zero, 0 and 0xffff patterns to VC3 registers 0-0x13 and reads
 * each straight back, all 16 bits. Register 7 is the SRAM address
 * (VC3_RAM_ADDR in the test's own names), so it reads back as the
 * address the write loaded.
 */
static void test_vc3_registers_read_back(void)
{
    mgras_reset();
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0));
    /* Walking one, walking zero, then 0 and 0xffff: 34 patterns. */
    for (int step = 0; step < 34; step++) {
        uint16_t pattern = step < 16 ? 1u << step :
                           step < 32 ? (uint16_t)~(1u << (step - 16)) :
                           step == 32 ? 0 : 0xffff;

        for (int reg = 0; reg <= 0x13; reg++) {
            qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(reg, pattern));
            qtest_writeb(qts, REG_VC3_INDEX, reg);
            g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, pattern);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_dac_gamma_identity_ramp_leaves_colors_unchanged(void)
{
    uint8_t ramp[3][256];

    mgras_reset();
    mgras_enter_context(qts);

    for (unsigned int channel = 0; channel < 3; channel++) {
        for (unsigned int i = 0; i < 256; i++) {
            ramp[channel][i] = i;
        }
    }

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 0x40, 0x11223300);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00112233);

    mgras_load_dac_gamma(qts, ramp);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00112233);
}

/*
 * The captured post-login desktop session's gamma table matches a
 * gamma-1.7 correction curve, 255*(i/255)^(1/1.7) rounded to the nearest
 * integer, at every one of its 256 captured entries within 1 and exactly
 * at the spot-checked entries 85 (-> 134) and 142 (-> 181). Only entry
 * 0x40's colour is asserted below; the rest of the curve stands in for
 * the untested majority of a real load.
 */
static const uint8_t mgras_test_gamma_1_7_curve[256] = {
    0, 10, 15, 19, 22, 25, 28, 31, 33, 36, 38, 40,
    42, 44, 46, 48, 50, 52, 54, 55, 57, 59, 60, 62,
    64, 65, 67, 68, 70, 71, 72, 74, 75, 77, 78, 79,
    81, 82, 83, 84, 86, 87, 88, 89, 91, 92, 93, 94,
    95, 97, 98, 99, 100, 101, 102, 103, 105, 106, 107, 108,
    109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120,
    121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132,
    133, 134, 135, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 144, 145, 146, 147, 148, 149, 150, 150, 151, 152, 153,
    154, 155, 156, 156, 157, 158, 159, 160, 160, 161, 162, 163,
    164, 164, 165, 166, 167, 168, 168, 169, 170, 171, 172, 172,
    173, 174, 175, 175, 176, 177, 178, 178, 179, 180, 181, 181,
    182, 183, 184, 184, 185, 186, 187, 187, 188, 189, 190, 190,
    191, 192, 192, 193, 194, 195, 195, 196, 197, 197, 198, 199,
    199, 200, 201, 202, 202, 203, 204, 204, 205, 206, 206, 207,
    208, 208, 209, 210, 210, 211, 212, 212, 213, 214, 214, 215,
    216, 216, 217, 218, 218, 219, 220, 220, 221, 222, 222, 223,
    224, 224, 225, 226, 226, 227, 227, 228, 229, 229, 230, 231,
    231, 232, 233, 233, 234, 234, 235, 236, 236, 237, 238, 238,
    239, 239, 240, 241, 241, 242, 242, 243, 244, 244, 245, 245,
    246, 247, 247, 248, 248, 249, 250, 250, 251, 251, 252, 253,
    253, 254, 254, 255,
};

static void test_dac_gamma_curve_applies_to_scanout(void)
{
    uint8_t ramp[3][256];

    mgras_reset();
    mgras_enter_context(qts);

    for (unsigned int channel = 0; channel < 3; channel++) {
        memcpy(ramp[channel], mgras_test_gamma_1_7_curve, 256);
    }

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 0x40, 0x55555500);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00555555);

    mgras_load_dac_gamma(qts, ramp);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00868686);
}


static void test_write_path_fixes_dont_raise_the_sticky_error(void)
{
    mgras_reset();

    qtest_writel(qts, REG_HQ_STATUS, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0x482b0400000000ULL);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_board_bringup_scratch_registers(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x02c574)) & 0x8, ==, 0x8);

    qtest_writel(qts, MGRAS_REG(0x061008), 0x1088);
    qtest_writel(qts, MGRAS_REG(0x061040), 0x210a);
    qtest_writel(qts, MGRAS_REG(0x060008), 2);
    qtest_writel(qts, MGRAS_REG(0x011088), 0x68f);
    qtest_writel(qts, MGRAS_REG(0x040918), 0x200000);
    qtest_writel(qts, MGRAS_REG(0x040928), 0);
    qtest_writeq(qts, MGRAS_REG(0x130400), 0x482b0400000000ULL);
    qtest_writel(qts, MGRAS_REG(0x011058), 0xffffffff);
    qtest_writel(qts, MGRAS_REG(0x070d88), 0x3000000);
    qtest_writel(qts, MGRAS_REG(0x071e80), 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x061008)), ==, 0x1088);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x061040)), ==, 0x210a);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x060008)), ==, 2);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x011088)), ==, 0x68f);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x040918)), ==, 0x200000);
    g_assert_cmphex(qtest_readq(qts, MGRAS_REG(0x130400)), ==,
                    0x482b0400000000ULL);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x011058)), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x070d88)), ==, 0x3000000);
}

/*
 * 0x070d88 (VC3_ARG) already stores and echoes a word write above; this
 * proves the newly accepted byte width behaves the same way.
 */
static void test_vc3_arg_byte_write(void)
{
    mgras_reset();

    qtest_writeb(qts, MGRAS_REG(0x070d88), 0x77);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x070d88)), ==, 0x77);
}


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
 * mgrasCopyPixels..OC's first kick pair does in the capture.
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
 * over it as PACKEDCOLOR 0x308080 (trace irix-20260927T035120-7dd191).
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

static void test_board_bringup_byte_poll_bit_position(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x071208)), ==, 0x08000000);
    g_assert_cmphex(qtest_readb(qts, MGRAS_REG(0x071208)) & 0x8, ==, 0x8);
}

/*
 * 0x071708 carries the CMAP and RA revisions and the no-video-option bit
 * in its top byte; the default SI board answers 0x83. Both the word and
 * the byte width IRIX uses give that byte without raising a guest error.
 */
static void test_unresolved_revision_byte_read(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x071708)), ==, 0x83000000);
    g_assert_cmphex(qtest_readb(qts, MGRAS_REG(0x071708)), ==, 0x83);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_board_info_byte_read_does_not_raise_error(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /*
     * The kernel's GFX_GETBOARDINFO handler performs a one-byte read here
     * on every OpenGL context query; the captured flow answers 0 and the
     * error state stays clear.
     */
    g_assert_cmphex(qtest_readb(qts, MGRAS_REG(0x071308)), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_quit(qts);
}

static void test_hq_upload_and_readback(void)
{
    mgras_reset();

    qtest_writel(qts, REG_HQ_UPLOAD, 0xff112233);
    qtest_writel(qts, REG_HQ_UPLOAD + 4, 0x00abcdef);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD), ==, 0x00112233);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD + 4), ==, 0x00abcdef);


    qtest_writel(qts, REG_HQ_UPLOAD, 0x00445566);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD), ==, 0x00445566);
}


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
 * MgrasCtxsw does not write or read a closing
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

static void test_pending_mask_and_reassert(void)
{
    mgras_reset();

    qtest_writel(qts, REG_IRQ_MASK, 0);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                     MGRAS_PENDING_FLAGS);


    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                     MGRAS_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==, 0);
}


#define HUB_WIDGET              9
#define HUB_REG_OFFSET          0x90
#define HQ_WIDGET               11
#define HQ_REG_OFFSET           0xa0

/*
 * Two of these keep a machine of their own. They assert that nothing has
 * been sent yet before arming a vector, and the model records the last
 * interrupt it sent in x-test-last-interrupt-addr and -value, which its
 * reset does not clear. Those two fields are diagnostic recordings, not
 * part of the register face, so a shared machine would carry an earlier
 * case's interrupt into this precondition.
 */
static void test_general_interrupt_vector(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    const unsigned int general_vector = 51;
    const unsigned int error_vector = 50;
    const uint64_t expected_addr =
        ((uint64_t)HQ_WIDGET << 48) | HQ_REG_OFFSET;

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, 0);

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS, general_vector << 8);

    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_FLAGS);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);


    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, general_vector | 0x100);


    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, general_vector);

    qtest_quit(qts);
}


/*
 * The IRIX 6.5.30 kernel connects its retrace handler to the third of the
 * board's crosstalk interrupts, places that vector in bits 23:16 of the HQ
 * vector word, and enables retrace interrupts by setting bit 0 of VC3
 * register 0x1e. It never sets bit 0 of the 0x011040 enable word (it writes
 * 0x3008a200 there), and the handler acknowledges nothing on the board: the
 * Hub's own pending bit is the only latch, which the kernel clears itself.
 * So the retrace interrupt is gated by the VC3 bit and arrives again at every
 * retrace. Aim the vector at the local Hub's interrupt-modify register and
 * watch the Hub's pending bit.
 */
#define HUB_BASE                UINT64_C(0x20001000000)
#define HUB_PI_INT_MODIFY       0x90
#define HUB_PI_INT_PEND0        0x98
/* The destination words the IRIX 6.5.30 kernel programs for node 0's Hub. */
#define KERNEL_HQ_DEST_UPPER    0x29008000
#define KERNEL_HQ_DEST_LOWER    0x01800090
#define KERNEL_IRQ_MASK         0x3008a200
#define VC3_CONTROL             0x1e
#define VC3_CONTROL_VINTR       0x1
#define VC3_CONTROL_DISPLAY     0x4

/*
 * A retrace period as frame clocks of a clock, and when retrace k falls
 * after the phase starts: exactly k frames, truncated to the nanosecond.
 * The default is the IRIX 6.5.30 1280x1024 60 Hz table (1065 lines of 840
 * VC3 clocks) at half its 107.25 MHz PLL output, about 59.943 Hz: the
 * timing section of docs/datasheets/mgras-raster-display.md in the Origami
 * repository.
 */
typedef struct MgrasFrameTime {
    uint64_t clocks;
    uint64_t hz;
} MgrasFrameTime;

static const MgrasFrameTime mgras_stock_frame = { 1065 * 840, 53625000 };

static int64_t mgras_retrace_at(const MgrasFrameTime *t, uint64_t k)
{
    return k * t->clocks * NANOSECONDS_PER_SECOND / t->hz;
}

/* From retrace k - 1 to retrace k. */
static int64_t mgras_retrace_gap(const MgrasFrameTime *t, uint64_t k)
{
    return mgras_retrace_at(t, k) - mgras_retrace_at(t, k - 1);
}

static uint64_t mgras_hub_read(uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 0, HUB_BASE + offset, bytes,
                                    sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void mgras_hub_write(uint64_t offset, uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, HUB_BASE + offset, bytes,
                                     sizeof(bytes)));
}

static void mgras_retrace_to_hub(unsigned int retrace_vector)
{
    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, KERNEL_HQ_DEST_UPPER);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, KERNEL_HQ_DEST_LOWER);
    qtest_writel(qts, REG_HQ_VECTORS, retrace_vector << 16);
    qtest_writel(qts, REG_IRQ_MASK, KERNEL_IRQ_MASK);
}

static void test_retrace_interrupt_vector(void)
{
    const unsigned int retrace_vector = 54;
    const uint64_t bit = UINT64_C(1) << retrace_vector;

    mgras_reset();
    mgras_retrace_to_hub(retrace_vector);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);

    qtest_clock_step(qts, mgras_retrace_gap(&mgras_stock_frame, 1));
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, bit);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, retrace_vector | 0x100);

    /* The kernel clears the Hub bit; the next retrace must set it again. */
    mgras_hub_write(HUB_PI_INT_MODIFY, retrace_vector);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
    qtest_clock_step(qts, mgras_retrace_gap(&mgras_stock_frame, 2));
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, bit);

    /* Clearing the VC3 enable stops further retrace interrupts. */
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL, VC3_CONTROL_DISPLAY));
    mgras_hub_write(HUB_PI_INT_MODIFY, retrace_vector);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 30);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
}

/* The 0x011040 enable word does not stand in for the VC3 enable. */
static void test_retrace_interrupt_needs_vc3_enable(void)
{
    const unsigned int retrace_vector = 54;
    const uint64_t bit = UINT64_C(1) << retrace_vector;

    mgras_reset();
    mgras_retrace_to_hub(retrace_vector);
    qtest_writel(qts, REG_IRQ_MASK, 0xffffffff);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL, VC3_CONTROL_DISPLAY));

    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 30);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
}

static uint64_t mgras_hub_pending(QTestState *q)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(q, 0, HUB_BASE + HUB_PI_INT_PEND0, bytes,
                                    sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void mgras_hub_clear(QTestState *q, unsigned int vector)
{
    uint8_t bytes[8];

    stq_be_p(bytes, vector);
    g_assert_true(qtest_memwrite_cpu(q, 0, HUB_BASE + HUB_PI_INT_MODIFY,
                                     bytes, sizeof(bytes)));
}

/*
 * Step to one nanosecond before the next retrace, see none, then step onto
 * it and see exactly one.
 */
static void mgras_expect_retrace_in(QTestState *q, int64_t ns,
                                    unsigned int vector)
{
    const uint64_t bit = UINT64_C(1) << vector;
    uint64_t before = mgras_qom_get_uint(q, "x-test-retraces");

    mgras_hub_clear(q, vector);
    qtest_clock_step(q, ns - 1);
    g_assert_cmphex(mgras_hub_pending(q) & bit, ==, 0);
    g_assert_cmpuint(mgras_qom_get_uint(q, "x-test-retraces"), ==, before);
    qtest_clock_step(q, 1);
    g_assert_cmphex(mgras_hub_pending(q) & bit, ==, bit);
    g_assert_cmpuint(mgras_qom_get_uint(q, "x-test-retraces"), ==,
                     before + 1);
}

static void mgras_retrace_to_hub_on(QTestState *q, unsigned int vector)
{
    qtest_writel(q, REG_HQ_VECTOR_DEST_UPPER, KERNEL_HQ_DEST_UPPER);
    qtest_writel(q, REG_HQ_VECTOR_DEST_LOWER, KERNEL_HQ_DEST_LOWER);
    qtest_writel(q, REG_HQ_VECTORS, vector << 16);
    qtest_writel(q, REG_IRQ_MASK, KERNEL_IRQ_MASK);
    qtest_writel(q, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
}

/* Retraces first .. last of t fall where t puts them, one at a time. */
static void mgras_expect_retraces(QTestState *q, const MgrasFrameTime *t,
                                  uint64_t first, uint64_t last,
                                  unsigned int vector)
{
    for (uint64_t k = first; k <= last; k++) {
        mgras_expect_retrace_in(q, mgras_retrace_gap(t, k), vector);
    }
}

/*
 * Before the guest loads a timing table the model runs the stock one, the
 * first retrace one period after reset. A hundred frames on, the phase is
 * still where the exact period puts it, so it does not drift by a rounded
 * period per frame. Enabling the display again part-way through a frame,
 * with nothing that changes the period, keeps the phase: the kernel
 * rewrites VC3 register 0x1e to stop and restart timing and to enable the
 * retrace interrupt.
 */
static void test_retrace_period_and_phase(void)
{
    const unsigned int retrace_vector = 54;
    const MgrasFrameTime *t = &mgras_stock_frame;
    int64_t half = mgras_retrace_gap(t, 106) / 2;

    mgras_reset();
    mgras_retrace_to_hub_on(qts, retrace_vector);
    mgras_expect_retraces(qts, t, 1, 4, retrace_vector);
    qtest_clock_step(qts, mgras_retrace_at(t, 104) - mgras_retrace_at(t, 4));
    mgras_expect_retraces(qts, t, 105, 105, retrace_vector);

    qtest_clock_step(qts, half);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retrace_in(qts, mgras_retrace_gap(t, 106) - half,
                            retrace_vector);
    mgras_expect_retraces(qts, t, 107, 107, retrace_vector);
}

static void mgras_vc3_sram_load(QTestState *q, uint16_t addr,
                                const uint16_t *words, size_t n)
{
    qtest_writel(q, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, addr));
    for (size_t i = 0; i < n; i++) {
        qtest_writew(q, REG_VC3_RAM, words[i]);
    }
}

/* One DAC register, as the kernel writes the PLL: number byte, data byte. */
static void mgras_dac_reg_write(QTestState *q, uint8_t reg, uint8_t value)
{
    qtest_writeb(q, REG_DAC_LUT_INDEX_LOW, reg);
    qtest_writeb(q, REG_DAC_LUT_DATA_2, value);
}

/*
 * A 640x480-shaped mode, 525 lines of 400 VC3 clocks, in a frame table at
 * 0x380 and two line sequences. The first mixes one- and two-halfword
 * entries and has an entry whose first halfword alone has bit 15 set,
 * which does not end the line; the second ends on a one-halfword entry.
 * Each sequence closes with its own address.
 */
static const uint16_t mgras_vt_line_a[] = {
    (100 << 8) | 0x05, 0x0003,          /* two halfwords, 100 clocks */
    0x0080 | (100 << 8),                /* one halfword, 100 clocks */
    0x8000 | (100 << 8) | 0x01, 0x0000, /* bit 15 on the first only */
    0x8080 | (100 << 8),                /* ends the line: 400 clocks */
    0x0000,
};
static const uint16_t mgras_vt_line_b[] = {
    0x0080 | (127 << 8), 0x0080 | (127 << 8), 0x0080 | (127 << 8),
    0x8080 | (19 << 8),                 /* 3 x 127 + 19 = 400 clocks */
    0x0010,
};
static const uint16_t mgras_vt_frame[] = {
    0x0000, 25, 0x0010, 500, 0x0000, 0,
};

/*
 * DAC r = 3, v = 13, cntl = 0x75, the IRIX 6.5.30 640x480 60 Hz setting:
 * 33 MHz x 61 / (5 x 8 x 2) = 25.1625 MHz, so the VC3 runs at 12.58125
 * MHz and the frame takes 210,000 of its clocks, about 59.911 Hz.
 */
static const MgrasFrameTime mgras_vga_frame = { 525 * 400, 12581250 };
/* The same table on the stock PLL bytes: 107.25 MHz, VC3 at 53.625 MHz. */
static const MgrasFrameTime mgras_vga_stock_pll_frame = { 525 * 400,
                                                          53625000 };

static void mgras_load_vga_timing(QTestState *q)
{
    mgras_vc3_sram_load(q, 0x0000, mgras_vt_line_a,
                        ARRAY_SIZE(mgras_vt_line_a));
    mgras_vc3_sram_load(q, 0x0010, mgras_vt_line_b,
                        ARRAY_SIZE(mgras_vt_line_b));
    mgras_vc3_sram_load(q, 0x0380, mgras_vt_frame,
                        ARRAY_SIZE(mgras_vt_frame));
    qtest_writel(q, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x0380));
    mgras_dac_reg_write(q, 0x0c, 3);
    mgras_dac_reg_write(q, 0x0f, 13);
    mgras_dac_reg_write(q, 0x09, 0x75);
    qtest_writel(q, REG_RESET_CONTROL, 0xe8000000);
}

/*
 * The period follows the timing the guest programs: the VC3 frame and
 * line tables give the frame length in VC3 clocks, the DAC PLL the pixel
 * clock, and enabling the display takes both. A new period restarts the
 * phase at that write. A table that gives no frame, and reset, return to
 * the stock period.
 */
static void test_retrace_period_follows_vc3_tables(void)
{
    const unsigned int retrace_vector = 54;

    mgras_reset();
    mgras_load_vga_timing(qts);
    mgras_retrace_to_hub_on(qts, retrace_vector);
    mgras_expect_retraces(qts, &mgras_vga_frame, 1, 3, retrace_vector);

    mgras_dac_reg_write(qts, 0x0c, 2);
    mgras_dac_reg_write(qts, 0x0f, 4);
    mgras_dac_reg_write(qts, 0x09, 0x91);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retraces(qts, &mgras_vga_stock_pll_frame, 1, 3,
                          retrace_vector);

    /* Word 0x1000 is still zero: an empty frame table. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x1000));
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retraces(qts, &mgras_stock_frame, 1, 2, retrace_vector);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x0380));
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retraces(qts, &mgras_vga_stock_pll_frame, 1, 1,
                          retrace_vector);
    mgras_reset();
    mgras_retrace_to_hub_on(qts, retrace_vector);
    mgras_expect_retraces(qts, &mgras_stock_frame, 1, 2, retrace_vector);
}

/*
 * The stream carries the retrace phase as the next retrace's time. The
 * destination's next retrace falls where the source's would have, the
 * ones after it one period apart, and none is made up for the frames the
 * destination's own clock never ran. Whether qtest's virtual clock travels
 * with the stream does not matter: the wait is taken from both clocks.
 */
/*
 * A table whose frame is shorter than the scanout's shortest retrace
 * period, 1 ms, runs the stock period, as a table with no frame does,
 * rather than stopping the retrace. One line of 10 VC3 clocks is 0.19 us
 * at the stock PLL. Enabling again with the same table keeps the phase.
 */
static void test_retrace_short_frame_runs_stock_period(void)
{
    const unsigned int retrace_vector = 54;
    const MgrasFrameTime *t = &mgras_stock_frame;
    static const uint16_t line[] = { 0x8080 | (10 << 8), 0x0040 };
    static const uint16_t frame[] = { 0x0040, 1, 0x0000, 0 };
    int64_t half = mgras_retrace_gap(t, 3) / 2;

    mgras_reset();
    mgras_load_vga_timing(qts);
    mgras_retrace_to_hub_on(qts, retrace_vector);
    mgras_expect_retraces(qts, &mgras_vga_frame, 1, 1, retrace_vector);

    mgras_vc3_sram_load(qts, 0x0040, line, ARRAY_SIZE(line));
    mgras_vc3_sram_load(qts, 0x0060, frame, ARRAY_SIZE(frame));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x0060));
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retraces(qts, t, 1, 2, retrace_vector);

    qtest_clock_step(qts, half);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retrace_in(qts, mgras_retrace_gap(t, 3) - half,
                            retrace_vector);
}

/*
 * One retrace migration: the source, started by src_env's binary if set,
 * runs src until half-way through its third frame; the destination must
 * fire that retrace where the source would have, then run dst.
 */
static void mgras_retrace_migration(const char *src_env, bool programmed,
                                    bool stale_table,
                                    const MgrasFrameTime *src,
                                    const MgrasFrameTime *dst)
{
    const unsigned int retrace_vector = 54;
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    int64_t reset_at, dest_now;

    tmpfs = g_dir_make_tmp("sn0-mgras-retrace-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext(src_env, MIGRATION_ARGS, NULL, true);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    /* An offset phase, so a destination that restarts it is caught. */
    reset_at = qtest_clock_step(from, 12345);
    qtest_system_reset(from);
    if (programmed) {
        mgras_load_vga_timing(from);
    }
    mgras_retrace_to_hub_on(from, retrace_vector);
    if (stale_table) {
        /*
         * Word 0x1000 is empty; without an enable write the model keeps
         * running the period it took from the table at 0x380.
         */
        qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x1000));
    }
    mgras_expect_retraces(from, src, 1, 2, retrace_vector);
    qtest_clock_step(from, mgras_retrace_gap(src, 3) / 2);

    sgi_migrate_stopped(from, uri);

    /*
     * qtest cannot step by zero; one nanosecond reads the clock. The
     * destination counts its frames from one period before the loaded
     * retrace, so the gaps after it are those after the first.
     */
    dest_now = qtest_clock_step(to, 1);
    mgras_expect_retrace_in(to,
                            reset_at + mgras_retrace_at(src, 3) - dest_now,
                            retrace_vector);
    g_assert_cmpuint(mgras_qom_get_uint(to, "x-test-retraces"), ==, 3);
    mgras_expect_retraces(to, dst, 2, 3, retrace_vector);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * Across two binaries, QTEST_QEMU_BINARY_SRC to QTEST_QEMU_BINARY_DST,
 * either of which may run the older exact 60 Hz period, check what does
 * not depend on the period: the stream loads, the retrace count travels,
 * and the pending retrace lands between the two models' third retrace.
 */
static void mgras_retrace_cross_binary_migration(void)
{
    const unsigned int retrace_vector = 54;
    const int64_t sixtieth = NANOSECONDS_PER_SECOND / 60;
    const MgrasFrameTime *t = &mgras_stock_frame;
    const uint64_t bit = UINT64_C(1) << retrace_vector;
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    int64_t reset_at, dest_now;

    tmpfs = g_dir_make_tmp("sn0-mgras-retrace-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_SRC", MIGRATION_ARGS, NULL,
                          true);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init_ext("QTEST_QEMU_BINARY_DST", dest_cmdline, NULL, true);

    reset_at = qtest_clock_step(from, 12345);
    qtest_system_reset(from);
    mgras_retrace_to_hub_on(from, retrace_vector);
    qtest_clock_step(from, 5 * sixtieth / 2);
    g_assert_cmpuint(mgras_qom_get_uint(from, "x-test-retraces"), ==, 2);

    sgi_migrate_stopped(from, uri);

    dest_now = qtest_clock_step(to, 1);
    mgras_hub_clear(to, retrace_vector);
    qtest_clock_step(to, reset_at + 3 * sixtieth - 1 - dest_now);
    g_assert_cmpuint(mgras_qom_get_uint(to, "x-test-retraces"), ==, 2);
    g_assert_cmphex(mgras_hub_pending(to) & bit, ==, 0);
    qtest_clock_step(to, mgras_retrace_at(t, 3) - 3 * sixtieth + 1);
    g_assert_cmpuint(mgras_qom_get_uint(to, "x-test-retraces"), ==, 3);
    g_assert_cmphex(mgras_hub_pending(to) & bit, ==, bit);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_retrace_phase_migration(void)
{
    if (g_getenv("QTEST_QEMU_BINARY_SRC") ||
        g_getenv("QTEST_QEMU_BINARY_DST")) {
        mgras_retrace_cross_binary_migration();
        return;
    }
    mgras_retrace_migration(NULL, false, false, &mgras_stock_frame,
                            &mgras_stock_frame);
}

/* The destination takes the period from the loaded VC3 and DAC state. */
static void test_retrace_programmed_period_migration(void)
{
    mgras_retrace_migration(NULL, true, false, &mgras_vga_frame,
                            &mgras_vga_frame);
}

/*
 * The guest moved VID_EP to another table after its last enable write, so
 * the migrated state no longer gives the period the source runs. The
 * stream carries that period, and the destination keeps it.
 */
static void test_retrace_running_period_migration(void)
{
    mgras_retrace_migration(NULL, true, true, &mgras_vga_frame,
                            &mgras_vga_frame);
}

/*
 * A stream before version 13 has no DAC registers, so the destination
 * cannot take a pixel clock from it and runs the stock period, not one
 * from DAC registers left at zero. Needs a binary that sends such a
 * stream, for example this tree built with the MGRAS description at
 * version 12; skipped otherwise, like the v9 format test below.
 */
static void test_retrace_pre_dac_stream_runs_stock_period(void)
{
    if (!g_getenv("QTEST_QEMU_BINARY_MGRAS_V12")) {
        g_test_skip("QTEST_QEMU_BINARY_MGRAS_V12 is not set");
        return;
    }
    mgras_retrace_migration("QTEST_QEMU_BINARY_MGRAS_V12", true, false,
                            &mgras_vga_frame, &mgras_stock_frame);
}


/*
 * The general line as the IRIX 6.5.30 kernel wires it: MgrasInitialize
 * enables 0x3008a200, and mgras_attach puts the general vector in bits
 * 15:8 of 0x011070. The Hub's pending bit for that vector shows the line.
 */
static void mgras_general_to_hub(unsigned int general_vector)
{
    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, KERNEL_HQ_DEST_UPPER);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, KERNEL_HQ_DEST_LOWER);
    qtest_writel(qts, REG_HQ_VECTORS, general_vector << 8);
    qtest_writel(qts, REG_IRQ_MASK, KERNEL_IRQ_MASK);
}

/*
 * MgrasInitialize writes 0x3008a200 to 0x011040, MgrasSchedSwapBuf adds 0x400
 * with a plain store, and MgrasGeneralInterrupt and the context switch's
 * tail remove bits through 0x011048.
 */
static void test_irq_enable_set_and_clear_ports(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==, 0);
    qtest_writel(qts, REG_IRQ_MASK, KERNEL_IRQ_MASK);
    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_SWAP);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==,
                    KERNEL_IRQ_MASK | MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_IRQ_ENABLE_CLEAR, MGRAS_PENDING_SWAP);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==, KERNEL_IRQ_MASK);
    qtest_writel(qts, REG_IRQ_ENABLE_CLEAR, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==, 0);
}

/*
 * MgrasSchedSwapBuf enables source 10 and writes the one word 0x3700; the
 * handler disables the source at 0x011048 and acknowledges 0x401.
 * That the word raises bit 10 is inferred; the model raises it on
 * consuming the word.
 */
static void test_swap_command_raises_interrupt_10(void)
{
    const unsigned int general_vector = 57;
    const uint64_t bit = UINT64_C(1) << general_vector;

    mgras_reset();
    mgras_general_to_hub(general_vector);
    mgras_enter_context(qts);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);

    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_SWAP);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_SWAP, ==, 0);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_SWAP_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_SWAP, ==,
                    MGRAS_PENDING_SWAP);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, bit);

    /* MgrasGeneralInterrupt serves the source it finds enabled and pending. */
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) &
                    qtest_readl(qts, REG_IRQ_MASK) & MGRAS_PENDING_SWAP, ==,
                    MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_IRQ_ENABLE_CLEAR, MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_SWAP | 0x1);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_SWAP, ==, 0);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
}

/* A swap word with source 10 disabled latches the source but stays quiet. */
static void test_swap_command_needs_enable(void)
{
    const unsigned int general_vector = 57;
    const uint64_t bit = UINT64_C(1) << general_vector;

    mgras_reset();
    mgras_general_to_hub(general_vector);
    mgras_enter_context(qts);

    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_SWAP_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_SWAP, ==,
                    MGRAS_PENDING_SWAP);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
}

/*
 * The kernel writes 0x400 to 0x011040 with a plain store before a swap, and
 * nothing in MgrasGeneralInterrupt, MgrasSchedSwapBufFinish or the next
 * MgrasCtxsw writes 0x011040 again before that switch sleeps on source 19.
 * If the store replaced the enable word, the source-19 wake would never
 * arrive.
 * Inferred: 0x011040 sets enables and 0x011048 clears them.
 */
static void test_swap_keeps_context_switch_enable(void)
{
    const unsigned int general_vector = 57;
    const uint64_t bit = UINT64_C(1) << general_vector;

    mgras_reset();
    mgras_general_to_hub(general_vector);
    mgras_enter_context(qts);

    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_SWAP);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==,
                    KERNEL_IRQ_MASK | MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_SWAP_TOKEN);
    qtest_writel(qts, REG_IRQ_ENABLE_CLEAR, MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_SWAP | 0x1);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==, KERNEL_IRQ_MASK);

    /* The line is low, so only the selection's source 19 can raise it. */
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, bit);
}

/* Its own machine for the same reason as the general-vector case. */
static void test_widget_error_interrupt_vector(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    const unsigned int error_vector = 52;
    const unsigned int hq_vector = 53;
    const uint64_t expected_addr =
        ((uint64_t)HUB_WIDGET << 48) | HUB_REG_OFFSET;

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS,
                 (hq_vector << 16) | (hq_vector << 8) | hq_vector);


    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_ERROR);
    mgras_enter_context(qts);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, 0);


    qtest_writel(qts, MGRAS_REG(0x000010), 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 1);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, error_vector | 0x100);

    qtest_quit(qts);
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

static void test_reset_clears_context_acknowledgements(void)
{
    mgras_reset();

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
    g_assert_true(qtest_qom_get_bool(qts, MGRAS_QOM_PATH,
                                     "x-test-first-ack"));
    g_assert_true(qtest_qom_get_bool(qts, MGRAS_QOM_PATH,
                                     "x-test-second-ack"));

    qtest_system_reset(qts);
    g_assert_false(qtest_qom_get_bool(qts, MGRAS_QOM_PATH,
                                      "x-test-first-ack"));
    g_assert_false(qtest_qom_get_bool(qts, MGRAS_QOM_PATH,
                                      "x-test-second-ack"));
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 1);
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


/*
 * The VC3 cursor position registers (VC3 registers 2 and 3) name the
 * glyph's top-left pixel plus 31 on each axis. The stock IRIX 6.5.30 server
 * writes the pointer position minus the glyph hotspot plus 31: for the shape
 * whose hotspot is its corner, a pointer at (48, 48) writes (79, 79). The
 * VC2's public Linux console driver adds the same 31 to its cursor Y. A
 * cursor drawn at the register value itself lands 31 pixels right of and
 * below the X pointer.
 */
static void mgras_load_cursor_rows(QTestState *qts, uint16_t base,
                                   const uint32_t mask[3])
{
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, base));
    for (unsigned int row = 0; row < 3; row++) {
        qtest_writew(qts, REG_VC3_RAM, mask[row] >> 16);
        qtest_writew(qts, REG_VC3_RAM, mask[row] & 0xffff);
    }
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, base));
}

static void test_cursor_position_origin(void)
{
    /* Row 0 marks column 0, row 1 column 11 and row 2 column 10. */
    const uint32_t mask[3] = {
        0x80000000, 0x80000000 >> 11, 0x80000000 >> 10
    };

    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    /* The cursor ink is entry 0x1c of WID 0's map, 29 under reset mode. */
    mgras_write_cmap_entry(qts, 29, 0x1c, 0x11223300);
    mgras_load_cursor_rows(qts, 0x400, mask);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x2));

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 100 + 31));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 200 + 31));
    g_assert_cmphex(mgras_test_display_pixel(qts, 100, 200), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(qts, 111, 201), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(qts, 131, 231), ==, 0);

    g_assert_cmphex(mgras_test_display_pixel(qts, 110, 202), ==, 0x00112233);

    /*
     * A glyph partly above and left of the screen keeps its visible part.
     * Row 2's bit lands at (-1, 1); drawn unclipped it would wrap to the
     * last column of row 0. Row 0 lands at y = -1, before the buffer, which
     * only a sanitizer build observes.
     */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 31 - 11));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 31 - 1));
    g_assert_cmphex(mgras_test_display_pixel(qts, 0, 0), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(qts, 1279, 0), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 100, 200), ==, 0);
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


static void test_cursor_composites_when_enabled(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    /* Entry bits 15:8 are blue, so this cursor ink is blue, not red. */
    mgras_write_cmap_position(qts, 0x001d, 28, 0x0000ff00);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0100));


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x8000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0142));
    qtest_writew(qts, REG_VC3_RAM, 0x4000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);

    /* The position registers name the glyph corner plus 31. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 50 + 31));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 60 + 31));


    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    g_assert_cmphex(mgras_screendump_pixel(qts, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 51, 61), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 52, 62), ==, 0);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0200));
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0100));
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0x000000ff);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    g_assert_cmphex(mgras_screendump_pixel(qts, 50, 60), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0000));
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0);
}

static void test_vc3_sram_round_trip(void)
{
    mgras_reset();

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x1234);
    qtest_writew(qts, REG_VC3_RAM, 0x5678);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x0100));
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0x5678);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x7fff));
    qtest_writew(qts, REG_VC3_RAM, 0xaaaa);
    qtest_writew(qts, REG_VC3_RAM, 0xbbbb);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x7fff));
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0xaaaa);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0xbbbb);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x0000));
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0xbbbb);
}

static void test_vc3_register_file(void)
{
    mgras_reset();

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x06, 0x2468));
    qtest_writeb(qts, REG_VC3_INDEX, 0x06);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 0x2468);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 100));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 200));
    qtest_writeb(qts, REG_VC3_INDEX, 0x02);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 100);
    qtest_writeb(qts, REG_VC3_INDEX, 0x03);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 200);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x0004));


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1f, 0x01ff));
    qtest_writeb(qts, REG_VC3_INDEX, 0x1f);
    /* Bits 7:6 read as the VC3 revision, 0 on the default SI board. */
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 0x013f);
}

static void test_board_inventory_registers(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_BOARD_INVENTORY), ==, 0x45000000);
    g_assert_cmphex(qtest_readb(qts, REG_BOARD_INVENTORY), ==, 0x45);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
    qtest_writel(qts, REG_RESET_CONTROL, 0xe8000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
}

/*
 * Expected identity values for each model variant: version bytes,
 * revisions, counts, and the name returned by hinv.
 */
typedef struct MgrasBoardExpect {
    const char *board;
    bool implemented;
    unsigned int ge, re, tram;
    const char *hinv;
    uint8_t version1, version0;
    uint8_t hq, ge_revision, re4, pp1, vc3, cmap, ra;
} MgrasBoardExpect;

static const MgrasBoardExpect mgras_board_expect[] = {
    { "si",    true,  1, 1, 0, "SI", 0x45, 0x0f, 2, 2, 2, 0, 0, 3, 0 },
    { "si-t",  false, 1, 1, 4, "SI with texture option",
      0x45, 0x0c, 2, 2, 2, 0, 0, 3, 0 },
    { "ssi",   true,  2, 2, 0, "SSI", 0x46, 0x0f, 2, 2, 2, 0, 0, 3, 0 },
    { "mxi",   false, 2, 2, 4, "MXI", 0x46, 0x00, 2, 2, 2, 0, 0, 3, 0 },
    { "esi",   true,  1, 1, 0, "ESI", 0x51, 0x0f, 2, 3, 2, 4, 0, 3, 0 },
    { "esi-t", false, 1, 1, 4, "ESI with texture option",
      0x51, 0x0c, 2, 3, 2, 4, 0, 3, 0 },
    { "essi",  true,  2, 2, 0, "ESSI", 0x52, 0x0f, 2, 3, 2, 4, 0, 3, 0 },
    { "emxi",  false, 2, 2, 4, "EMXI", 0x52, 0x00, 2, 3, 2, 4, 0, 3, 0 },
};

#define REG_GE_STATUS          MGRAS_REG(0x028000)
#define REG_GE_REVISION        MGRAS_REG(0x028004)
#define REG_CMAP_RA_REVISION   MGRAS_REG(0x071708)
#define XMAP_INDEX_PP1_REVISION 4

/*
 * The name IRIX 6.5.x hinv prints for the counts MgrasGatherInfo decodes
 * from the two version bytes, and the E-series marker the kernel sets from
 * a GE revision of 3 or more. Products 2 and 3 only: RE count equals GE count,
 * and a TRAM path holds 4 TRAMs unless its field is 3.
 */
static const char *mgras_decode_hinv_name(uint8_t version1, uint8_t version0,
                                          uint8_t ge_revision,
                                          unsigned int *ge, unsigned int *re,
                                          unsigned int *tram0,
                                          unsigned int *tram1)
{
    unsigned int product = (version1 >> 5) & 3;
    bool e_series = ge_revision >= 3;

    g_assert_true(product == 2 || product == 3);
    *ge = version1 & 3;
    *re = *ge;
    *tram0 = (version0 & 3) == 3 ? 0 : 4;
    *tram1 = ((version0 >> 2) & 3) == 3 ? 0 : 4;
    if (*ge == 1 && *re == 1) {
        if (*tram0) {
            return e_series ? "ESI with texture option" :
                   "SI with texture option";
        }
        return e_series ? "ESI" : "SI";
    }
    if (*ge == 2 && *re == 2) {
        if (*tram0) {
            return e_series ? "EMXI" : "MXI";
        }
        return e_series ? "ESSI" : "SSI";
    }
    return "Impact";
}

static QTestState *mgras_board_init(const MgrasBoardExpect *e)
{
    g_autofree char *args = g_strdup_printf(
        MACHINE_ARGS " -global sgi-mgras.board=%s%s", e->board,
        e->implemented ? "" : " -global sgi-mgras.x-unimplemented-board=on");

    return qtest_init(args);
}

/*
 * Every identity read the IRIX kernel makes, at the widths it makes them,
 * answers the datasheet's value for the selected board, and those values
 * decode to the board's counts and hinv name.
 */
static void test_board_identity(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    QTestState *s = mgras_board_init(e);
    uint8_t version1, version0, ge_revision;
    unsigned int ge, re, tram0, tram1;
    const char *name;

    version1 = qtest_readl(s, REG_BOARD_INVENTORY) >> 24;
    version0 = qtest_readl(s, REG_RESET_CONTROL) >> 24;
    g_assert_cmphex(version1, ==, e->version1);
    g_assert_cmphex(qtest_readb(s, REG_BOARD_INVENTORY), ==, e->version1);
    g_assert_cmphex(version0, ==, e->version0);
    g_assert_cmphex(qtest_readl(s, REG_BOARD_INVENTORY) & 0x00ffffff, ==, 0);
    g_assert_cmphex(qtest_readl(s, REG_RESET_CONTROL) & 0x00ffffff, ==, 0);

    g_assert_cmphex(qtest_readl(s, MGRAS_REG(0x000000)) >> 28, ==, e->hq);

    /* The kernel reads 0x028000 first and keeps only 0x028004's low byte. */
    g_assert_cmphex(qtest_readl(s, REG_GE_STATUS), ==, 0);
    ge_revision = qtest_readl(s, REG_GE_REVISION) & 0xff;
    g_assert_cmphex(ge_revision, ==, e->ge_revision);

    g_assert_cmphex((qtest_readl(s, REG_DIRECT_COPY_READY) >> 4) & 0xf, ==,
                    e->re4);

    qtest_writeb(s, REG_XMAP_INDEX, XMAP_INDEX_PP1_REVISION);
    g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3) & 7, ==, e->pp1);

    qtest_writeb(s, REG_VC3_INDEX, 0x1f);
    g_assert_cmphex((qtest_readw(s, REG_VC3_DATA) >> 6) & 3, ==, e->vc3);

    g_assert_cmphex(qtest_readb(s, REG_CMAP_RA_REVISION) & 7, ==, e->cmap);
    g_assert_cmphex((qtest_readb(s, REG_CMAP_RA_REVISION) >> 4) & 7, ==,
                    e->ra);
    /* Set: no video option board, as on every captured Origin board. */
    g_assert_cmphex(qtest_readb(s, REG_CMAP_RA_REVISION) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readl(s, REG_CMAP_RA_REVISION), ==,
                    (uint32_t)qtest_readb(s, REG_CMAP_RA_REVISION) << 24);

    name = mgras_decode_hinv_name(version1, version0, ge_revision, &ge, &re,
                                  &tram0, &tram1);
    g_assert_cmpuint(ge, ==, e->ge);
    g_assert_cmpuint(re, ==, e->re);
    g_assert_cmpuint(tram0, ==, e->tram);
    /* One path on a one-GE board; both alike on a two-GE board. */
    g_assert_cmpuint(tram1, ==, e->ge == 2 ? e->tram : 0);
    g_assert_cmpstr(name, ==, e->hinv);

    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * 0x028004 carries the GE revision from reset until the first 0xa2 query,
 * the kernel's or a client's, which replaces it with the answer (zero); a
 * reset brings the revision back. Uses the E-series board so the revision
 * cannot be mistaken for the answer.
 */
static void test_board_ge_revision_until_query(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-mgras.board=esi");

    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 3);
    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 3);

    mgras_enter_context(s);
    qtest_writel(s, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    qtest_writel(s, REG_CONTEXT_COMMAND, MGRAS_QUERY_TOKEN);
    qtest_writel(s, REG_CONTEXT_COMMAND, 0x3a);
    qtest_writel(s, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 3);
    qtest_writel(s, REG_CONTEXT_COMMAND, 1);
    g_assert_cmphex(qtest_readl(s, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 0);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 3);
    qtest_quit(s);
}

/*
 * Without a board property the model is the textureless SI, the board
 * every existing Origin 2000 run has used.
 */
static void test_board_default_is_si(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_BOARD_INVENTORY), ==, 0x45000000);
    g_assert_cmphex(qtest_readl(qts, REG_GE_REVISION), ==, 2);
}

/*
 * A rejection reports through error_report and exits; it must not reach
 * the guest as a signal (SIGABRT from a failed assertion, SIGSEGV from a
 * wild pointer). WIFSIGNALED tells the two apart where a bare nonzero
 * exit status cannot. Reimplemented from tests/qtest/sn-builder-test.c's
 * own assert_qemu_rejects(): a qtest binary cannot link another qtest
 * file's static helper.
 */
static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_false(WIFSIGNALED(wait_status));
    g_assert_nonnull(strstr(error_output, message));
}

/*
 * A board with TRAM is refused at realize, naming texture as what the model
 * lacks, unless x-unimplemented-board asks for it. "-qtest
 * stdio -accel qtest" gets past the machine's stock-PROM-only check the
 * same way sn0-origin2000-kona-test.c's rejection cases do.
 */
static void test_board_refused(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    g_autofree char *global = g_strdup_printf("sgi-mgras.board=%s", e->board);
    g_autofree char *message = g_strdup_printf("board '%s' (%s) needs",
                                               e->board, e->hinv);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-global", global,
        "-device", "sgi-mgras,slot=io3",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };

    /* Texture is the only thing any board still lacks. */
    g_assert_cmpuint(e->tram, >, 0);
    assert_qemu_rejects(args, message);
    assert_qemu_rejects(args, "needs texture (TE1 and TRAM), which");
}

/*
 * Scanout builds each row's window IDs in stack buffers of
 * SGI_MGRAS_MAX_WIDTH entries, and realize refuses a descriptor that names
 * a wider screen rather than let scanout run past them. Every descriptor
 * states 1280x1024, so every board realizes, and a fill of the whole
 * screen reaches the display at both ends of the scanned rows.
 */
static void test_board_screen_fits_row_buffers(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    QTestState *s = mgras_board_init(e);

    mgras_enter_context(s);
    qtest_writel(s, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(s, 29, 0x40, 0x11223300);
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(s, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(s, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_BLOCK_START, 0));
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (1279u << 16) | 1023u));
    g_assert_cmphex(mgras_test_display_pixel(s, 0, 0), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(s, 1279, 1023), ==, 0x00112233);
    /* Past the screen there is no pixel. */
    g_assert_cmphex(mgras_test_display_pixel(s, 1280, 1023), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

static void test_board_unknown_refused(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000",
        "-global", "sgi-mgras.board=vpro",
        "-device", "sgi-mgras,slot=io3",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args, "unknown board 'vpro'");
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


static void test_int_dest_alt_offset_aliases(void)
{
    mgras_reset();

    qtest_writel(qts, REG_INT_DEST_UPPER_ALT, 0x12340000);
    qtest_writel(qts, REG_INT_DEST_LOWER_ALT, 0x5678);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_UPPER), ==, 0x12340000);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_LOWER), ==, 0x5678);

    qtest_writel(qts, REG_INT_DEST_UPPER, 0xabcd0000);
    qtest_writel(qts, REG_INT_DEST_LOWER, 0xef01);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_UPPER_ALT), ==, 0xabcd0000);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_LOWER_ALT), ==, 0xef01);
}


static void test_hq_config_and_enable_registers(void)
{
    mgras_reset();

    qtest_writel(qts, REG_HQ_CONFIG_A, 0x11000f);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_CONFIG_A), ==, 0x11000f);
    qtest_writel(qts, REG_HQ_CONFIG_B, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_CONFIG_B), ==, 0xffffffff);

    qtest_writel(qts, REG_HQ_ENABLE, 0x1f8);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1f8);

    qtest_writel(qts, REG_HQ_ENABLE, (0x1f8 & ~8) | 0x10);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1f0);

    qtest_writel(qts, REG_HQ_ENABLE_CLEAR, 0x10);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1e0);


    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
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


static void test_hq_vector_registers(void)
{
    mgras_reset();

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, 0x2000f000);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, 0x9000);
    qtest_writel(qts, REG_HQ_VECTORS, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTOR_DEST_UPPER), ==,
                     0x2000f000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTOR_DEST_LOWER), ==, 0x9000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTORS), ==, 0x01020304);
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
 * __Mgr_ReturnMode clears flag 17, writes 0xa20c and three arguments, polls
 * 0x020010 until bit 17 is set, then reads the answer at 0x028004 once.
 * The answer's value is unknown; the model answers zero.
 */
static void test_hq_query_sets_flag_after_arguments(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_QUERY_TOKEN);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x3a);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    0);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    g_assert_cmphex(qtest_readl(qts, REG_QUERY_ANSWER), ==, 0);

    /* The arguments were consumed: the next word is a header again. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

/* Other class-0 opcodes, the swap word among them, leave flag 17 alone. */
static void test_hq_query_flag_only_from_query(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_SWAP_TOKEN);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x00003600);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    0);
}

/*
 * An inline data block's header has bit 31 set and a 20-bit byte length.
 * The GL client's first GE table load is 0x80000214: five descriptor words
 * and 128 data words. Filling the block with fence tokens shows whether
 * any of it parsed as headers.
 */
static void test_inline_block_uses_20_bit_length(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x80000214);
    for (int i = 0; i < 0x214 / 4; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}


/*
 * The OpenGL client's own words on the main port, as the IRIX 6.5.30
 * IP27GR532 libGLcore.so frames them: glViewport 0x3310,
 * glClearColor 0xba10, glClear's colour token 0x1500,
 * glShadeModel 0x4704, glColor3f 0x2000020c, glBegin and
 * glEnd for GL_TRIANGLES 0x1a00 and 0x2400, glVertex2f 0x40000008 and
 * glFlush 0x3600.
 */
#define GL_VIEWPORT_HDR       0x00003310
#define GL_CLEAR_COLOR_HDR    0x0000ba10
#define GL_CLEAR_COLOR_BUFFER 0x00001500
#define GL_SHADE_MODEL_HDR    0x00004704
#define GL_COLOR3F_HDR        0x2000020c
#define GL_BEGIN_TRIANGLES    0x00001a00
#define GL_END_TRIANGLES      0x00002400
#define GL_VERTEX2F_HDR       0x40000008
#define GL_FLUSH              0x00003600
#define GL_FLAT               0x1d00

/*
 * The 63-word context image the IRIX kernel wrote for the probe's GL
 * context, a 128x128 window whose top-left pixel is at screen (256, 256)
 * (run irix-20260927T135825-92207b, seq 3959112). Word 2 is the window's
 * lower-left corner as (y << 16) | x with y counted up from the bottom of
 * the screen, word 3 is 0x11, and words 11 and 12 are the window's column
 * and row ranges in the 0x147/0x148 layout. The other words are carried
 * over unchanged.
 */
static const uint32_t gl_context_image[MGRAS_CONTEXT_WORDS] = {
    0x39e9, 0x2c000000, 0x2800100, 0x11, 0, 0x124f8, 0xfb50414, 0, 0,
    0xffa3, 0, 0x100017f, 0x28002ff, 0x90240, 0x2800100, 0x90240, 1, 1,
    0x1000100, 0x17f017f, 0x800080, 0x11400000, 8, 0, 0xcac, 0xc004504, 0,
    0xff, 0xff, 0x10, 0, 0, 0, 0, 0, 0, 0x4ff, 0x3ff, 0xffffff, 0xffffff,
};

/*
 * Switch to the GL context whose image word 0 is slot and whose window has
 * its lower-left pixel at column x and at row y counted from the bottom of
 * the screen.
 */
static void mgras_enter_gl_slot(QTestState *qts, uint32_t slot, uint32_t x,
                                uint32_t y, uint32_t w, uint32_t h,
                                uint32_t clip)
{
    uint32_t image[MGRAS_CONTEXT_WORDS];

    memcpy(image, gl_context_image, sizeof(image));
    image[0] = slot;
    image[2] = image[14] = (y << 16) | x;
    image[3] = clip;
    image[11] = (x << 16) | (x + w - 1);
    image[12] = (y << 16) | (y + h - 1);

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, image[i]);
    }
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                    MGRAS_PENDING_CONTEXT);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
}

static void mgras_enter_gl_window(QTestState *qts, uint32_t x, uint32_t y,
                                  uint32_t w, uint32_t h, uint32_t clip)
{
    mgras_enter_gl_slot(qts, gl_context_image[0], x, y, w, h, clip);
}

static void gl_word(QTestState *qts, uint32_t word)
{
    qtest_writel(qts, REG_CONTEXT_COMMAND, word);
}

static void gl_float(QTestState *qts, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    gl_word(qts, bits);
}

static void gl_viewport(QTestState *qts, uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h)
{
    gl_word(qts, GL_VIEWPORT_HDR);
    gl_word(qts, x);
    gl_word(qts, y);
    gl_word(qts, w);
    gl_word(qts, h);
}

static void gl_clear_color(QTestState *qts, float r, float g, float b,
                           float a)
{
    gl_word(qts, GL_CLEAR_COLOR_HDR);
    gl_float(qts, r);
    gl_float(qts, g);
    gl_float(qts, b);
    gl_float(qts, a);
}

static void gl_color3f(QTestState *qts, float r, float g, float b)
{
    gl_word(qts, GL_COLOR3F_HDR);
    gl_float(qts, r);
    gl_float(qts, g);
    gl_float(qts, b);
}

static void gl_vertex2f(QTestState *qts, float x, float y)
{
    gl_word(qts, GL_VERTEX2F_HDR);
    gl_float(qts, x);
    gl_float(qts, y);
}

/* One screendump, 0xRRGGBB per pixel. */
static uint32_t *mgras_screendump(QTestState *qts)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    uint32_t *rgb = g_new(uint32_t, 1280 * 1024);
    const uint8_t *pixels;
    gsize length;
    int fd;

    fd = g_file_open_tmp("sgi-mgras-gl-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    qtest_qmp_assert_success(qts,
        "{'execute': 'screendump', 'arguments': {'filename': %s}}", path);
    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_no_error(error);
    unlink(path);
    g_assert_true(g_str_has_prefix(contents, "P6\n1280 1024\n255\n"));
    g_assert_cmpuint(length, ==, strlen("P6\n1280 1024\n255\n") +
                     1280 * 1024 * 3);
    pixels = (const uint8_t *)contents + strlen("P6\n1280 1024\n255\n");
    for (int i = 0; i < 1280 * 1024; i++) {
        rgb[i] = (pixels[i * 3] << 16) | (pixels[i * 3 + 1] << 8) |
                 pixels[i * 3 + 2];
    }
    return rgb;
}

/*
 * The probe's clear case: glViewport(0, 0, 128, 128), glClearColor(1, 0,
 * 1, 1), glClear(GL_COLOR_BUFFER_BIT), glFlush, in a 128x128 window whose
 * top-left pixel is at (256, 256). Every window pixel becomes magenta and
 * nothing outside it changes (OpenGL 1.2.1, 4.2.3: a clear passes only the
 * pixel ownership, scissor and dither steps, and colour 1.0 converts to
 * all ones by 2.13.9).
 */
static void test_gl_clear_fills_the_window(void)
{
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_FLUSH);

    g_assert_cmphex(mgras_test_pixel(qts, 256, 256), ==, 0xff00ff);
    g_assert_cmphex(mgras_test_format(qts, 256, 256), ==,
                    MGRAS_PIXEL_FORMAT_RGB);
    rgb = mgras_screendump(qts);
    for (int y = 250; y < 390; y++) {
        for (int x = 250; x < 390; x++) {
            bool inside = x >= 256 && x < 384 && y >= 256 && y < 384;

            if (rgb[y * 1280 + x] != (inside ? 0xff00ff : 0)) {
                g_error("pixel (%d, %d) is 0x%06x", x, y, rgb[y * 1280 + x]);
            }
        }
    }
}

/*
 * A clear covers the window, not the viewport (OpenGL 1.2.1, 4.2.3), and
 * lands wherever the context image puts the window.
 */
static void test_gl_clear_ignores_the_viewport(void)
{
    mgras_reset();

    /* Rows 200-231 from the bottom are screen rows 792-823. */
    mgras_enter_gl_window(qts, 100, 200, 64, 32, 0x11);
    gl_viewport(qts, 0, 0, 8, 8);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);

    g_assert_cmphex(mgras_test_pixel(qts, 100, 792), ==, 0x00ff00);
    g_assert_cmphex(mgras_test_pixel(qts, 163, 823), ==, 0x00ff00);
    g_assert_cmphex(mgras_test_pixel(qts, 163, 792), ==, 0x00ff00);
    g_assert_cmphex(mgras_test_pixel(qts, 99, 792), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 164, 823), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 791), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 824), ==, 0);
}

/*
 * Without the image's window rectangle (word 3 not 0x11) the model does not
 * know which pixels the window owns, so the clear paints nothing.
 */
static void test_gl_clear_needs_the_window_rectangle(void)
{
    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 1, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);

    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0);
    g_assert_cmphex(mgras_test_format(qts, 300, 300), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
}

/*
 * The server gives the probe's window display ID 14 and XMAP mode 0x1408
 * (run irix-20260927T135825-92207b, seq 3899859-3899880). Bits 9:5 of that
 * mode are zero, so an indexed pixel left under the window resolves
 * through colour map 0, which the server never loads, and shows black. A
 * GL clear stores direct colour, which bypasses the map.
 */
static void test_gl_clear_shows_through_a_direct_colour_display_id(void)
{
    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    /* WID 0's reset mode 0x7a4 selects map 29; make index 0 white there. */
    mgras_write_cmap_entry(qts, 29, 0, 0xffffff00);
    qtest_writeb(qts, REG_XMAP_INDEX, 14 << 2);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x1408);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256, 256), ==, 0xffffff);
    mgras_tag_wid(qts, 256, 256, 259, 256, 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256, 256), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 260, 256), ==, 0xffffff);

    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256, 256), ==, 0xff00ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 260, 256), ==, 0xff00ff);
}

/*
 * Expected yellow spans for the triangle test cases in a 128x128 window.
 * Every other window pixel is the clear color.
 */
typedef struct GlSpan {
    uint8_t row;
    uint8_t left;
    uint8_t right;
} GlSpan;

static const GlSpan gl_triangle_spans[] = {
    {18, 55, 56}, {19, 55, 57}, {20, 55, 57}, {21, 54, 58}, {22, 54, 59},
    {23, 53, 59}, {24, 53, 60}, {25, 52, 60}, {26, 52, 61}, {27, 52, 62},
    {28, 51, 62}, {29, 51, 63}, {30, 50, 64}, {31, 50, 64}, {32, 49, 65},
    {33, 49, 66}, {34, 49, 66}, {35, 48, 67}, {36, 48, 68}, {37, 47, 68},
    {38, 47, 69}, {39, 47, 69}, {40, 46, 70}, {41, 46, 71}, {42, 45, 71},
    {43, 45, 72}, {44, 44, 73}, {45, 44, 73}, {46, 44, 74}, {47, 43, 75},
    {48, 43, 75}, {49, 42, 76}, {50, 42, 77}, {51, 41, 77}, {52, 41, 78},
    {53, 41, 78}, {54, 40, 79}, {55, 40, 80}, {56, 39, 80}, {57, 39, 81},
    {58, 39, 82}, {59, 38, 82}, {60, 38, 83}, {61, 37, 84}, {62, 37, 84},
    {63, 36, 85}, {64, 36, 86}, {65, 36, 86}, {66, 35, 87}, {67, 35, 88},
    {68, 34, 88}, {69, 34, 89}, {70, 33, 89}, {71, 33, 90}, {72, 33, 91},
    {73, 32, 91}, {74, 32, 92}, {75, 31, 93}, {76, 31, 93}, {77, 31, 94},
    {78, 30, 95}, {79, 30, 95}, {80, 29, 96}, {81, 29, 97}, {82, 28, 97},
    {83, 28, 98}, {84, 28, 98}, {85, 27, 99}, {86, 27, 100}, {87, 26, 100},
    {88, 26, 101}, {89, 25, 102}, {90, 25, 102}, {91, 25, 103}, {92, 24, 104},
    {93, 24, 104}, {94, 23, 105}, {95, 23, 106}, {96, 23, 106}, {97, 22, 107},
    {98, 22, 107}, {99, 21, 108}, {100, 21, 109}, {101, 20, 109},
    {102, 20, 110}, {103, 20, 111}, {104, 19, 105}, {105, 19, 93},
    {106, 18, 81}, {107, 18, 69}, {108, 17, 57}, {109, 17, 45}, {110, 17, 33},
    {111, 16, 21},
};
static const GlSpan gl_triangle_moved_spans[] = {
    {26, 87, 87}, {27, 86, 88}, {28, 85, 88}, {29, 84, 88}, {30, 83, 89},
    {31, 83, 89}, {32, 82, 89}, {33, 81, 90}, {34, 80, 90}, {35, 79, 90},
    {36, 78, 90}, {37, 78, 91}, {38, 77, 91}, {39, 76, 91}, {40, 75, 92},
    {41, 74, 92}, {42, 74, 92}, {43, 73, 93}, {44, 72, 93}, {45, 71, 93},
    {46, 70, 94}, {47, 69, 94}, {48, 69, 94}, {49, 68, 94}, {50, 67, 95},
    {51, 66, 95}, {52, 65, 95}, {53, 64, 96}, {54, 64, 96}, {55, 63, 96},
    {56, 62, 97}, {57, 61, 97}, {58, 60, 97}, {59, 59, 97}, {60, 59, 98},
    {61, 58, 98}, {62, 57, 98}, {63, 56, 99}, {64, 55, 99}, {65, 54, 99},
    {66, 54, 100}, {67, 53, 100}, {68, 52, 100}, {69, 51, 101}, {70, 50, 101},
    {71, 50, 101}, {72, 49, 101}, {73, 48, 102}, {74, 47, 102}, {75, 46, 102},
    {76, 45, 103}, {77, 45, 103}, {78, 44, 103}, {79, 43, 104}, {80, 42, 104},
    {81, 41, 104}, {82, 40, 104}, {83, 40, 105}, {84, 39, 105}, {85, 38, 105},
    {86, 37, 106}, {87, 36, 106}, {88, 35, 106}, {89, 35, 107}, {90, 34, 107},
    {91, 33, 107}, {92, 32, 108}, {93, 31, 108}, {94, 30, 108}, {95, 30, 108},
    {96, 29, 109}, {97, 28, 109}, {98, 27, 109}, {99, 26, 110}, {100, 26, 110},
    {101, 25, 110}, {102, 24, 111}, {103, 23, 111}, {104, 22, 105},
    {105, 21, 93}, {106, 21, 81}, {107, 20, 69}, {108, 19, 57}, {109, 18, 45},
    {110, 17, 33}, {111, 16, 21},
};

static void gl_check_spans(const uint32_t *rgb, const GlSpan *spans,
                           size_t count, uint32_t ink, uint32_t paper)
{
    size_t next = 0;
    unsigned int drawn = 0;

    for (int row = 0; row < 128; row++) {
        const GlSpan *span = next < count && spans[next].row == row ?
                             &spans[next++] : NULL;

        for (int col = 0; col < 128; col++) {
            bool in = span && col >= span->left && col <= span->right;
            uint32_t got = rgb[(256 + row) * 1280 + 256 + col];

            if (got != (in ? ink : paper)) {
                g_error("window pixel (%d, %d) is 0x%06x, expected 0x%06x",
                        col, row, got, in ? ink : paper);
            }
            drawn += in;
        }
    }
    g_assert_cmpuint(next, ==, count);
    g_assert_cmpuint(drawn, >, 0);
}

static void gl_draw_probe_triangle(QTestState *qts, const float v[3][2])
{
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_SHADE_MODEL_HDR);
    gl_word(qts, GL_FLAT);
    gl_color3f(qts, 1, 1, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        gl_vertex2f(qts, v[i][0], v[i][1]);
    }
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);
}

/*
 * The probe's triangle case: window vertices (16, 16), (112, 24) and
 * (56, 111) as normalized device coordinates. A pixel is drawn exactly
 * when its centre lies inside (OpenGL 1.2.1, 3.5.1); no centre lies on an
 * edge, so the edge rule cannot change the census.
 */
static void test_gl_flat_triangle_matches_the_reference(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { 0.75f, -0.625f }, { -0.125f, 0.734375f },
    };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_draw_probe_triangle(qts, v);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
}

/* Moving the third vertex to (88, 103) changes exactly the census. */
static void test_gl_flat_triangle_moved_vertex(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { 0.75f, -0.625f }, { 0.375f, 0.609375f },
    };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_draw_probe_triangle(qts, v);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_moved_spans,
                   ARRAY_SIZE(gl_triangle_moved_spans), 0xffff00, 0x0000ff);
}

/*
 * Culling is off by default (OpenGL 1.2.1, 3.5.1), so the reversed
 * winding draws the same pixels.
 */
static void test_gl_flat_triangle_reversed_winding(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { -0.125f, 0.734375f }, { 0.75f, -0.625f },
    };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_draw_probe_triangle(qts, v);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
}

/*
 * A flat-shaded independent triangle takes the colour current at its
 * third vertex (OpenGL 1.2.1, 2.13.7 and Table 2.9).
 */
static void test_gl_flat_triangle_takes_the_third_vertex_colour(void)
{
    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_word(qts, GL_SHADE_MODEL_HDR);
    gl_word(qts, GL_FLAT);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex2f(qts, -1, -1);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex2f(qts, 1, -1);
    gl_color3f(qts, 0, 0, 1);
    gl_vertex2f(qts, -1, 1);
    gl_word(qts, GL_END_TRIANGLES);

    /* Window pixel (10, 10) from the bottom is screen (266, 373). */
    g_assert_cmphex(mgras_test_pixel(qts, 266, 373), ==, 0x0000ff);
    g_assert_cmphex(mgras_test_format(qts, 266, 373), ==,
                    MGRAS_PIXEL_FORMAT_RGB);
    /* Above the diagonal nothing is drawn. */
    g_assert_cmphex(mgras_test_pixel(qts, 373, 266), ==, 0);
}

static void gl_vertex4f(QTestState *qts, float x, float y, float z, float w)
{
    gl_word(qts, 0x00000010);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
    gl_float(qts, w);
}

/* Window pixel (x, y), y counted up from the bottom of the probe window. */
static uint32_t gl_window_pixel(QTestState *qts, int x, int y)
{
    return mgras_test_pixel(qts, 256 + x, 383 - y);
}

/*
 * Draw one white triangle given in window coordinates of a 128x128
 * viewport, where NDC is x / 64 - 1, into a freshly reset probe window.
 */
static void gl_draw_window_triangle(QTestState *qts, const double v[3][2])
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        gl_vertex2f(qts, v[i][0] / 64 - 1, v[i][1] / 64 - 1);
    }
    gl_word(qts, GL_END_TRIANGLES);
}

/*
 * The first vertex sits at x 10.5 + 3/1024, a quarter of a subpixel off
 * the 1/256 grid. Rounded to the nearest subpixel it moves to 10.5 +
 * 1/256, and the edge to (10.5 - 20/256, 40) then passes right of the
 * centres of window pixels (10, 0) and (10, 1), which stay undrawn.
 * Unsnapped, the edge passes left of (10, 1)'s centre and draws it;
 * truncated to 10.5 it draws both. No centre lies on an edge after
 * rounding, so the edge rule plays no part.
 */
static void test_gl_triangle_snaps_to_the_nearest_subpixel(void)
{
    static const double v[3][2] = {
        { 10.5 + 3.0 / 1024, 0 }, { 10.5 - 20.0 / 256, 40 }, { 30, 20 },
    };

    gl_draw_window_triangle(qts, v);
    g_assert_cmphex(gl_window_pixel(qts, 10, 0), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 10, 1), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 11, 1), ==, 0xffffff);
}

/*
 * Two pairs of triangles share an edge through pixel centres: a diagonal
 * from (0, 0) to (16, 16) and a horizontal edge at y 8.5. OpenGL 1.2.1,
 * 3.5.1 requires each such centre to be drawn by exactly one of the pair.
 * The model gives it to the triangle for which the edge is a left edge
 * (the one below and right of the diagonal) or a top edge (the one below
 * the horizontal).
 */
static unsigned int gl_count_drawn(QTestState *qts, const double v[3][2],
                                   bool diagonal)
{
    unsigned int drawn = 0;

    gl_draw_window_triangle(qts, v);
    for (int i = 0; i < 16; i++) {
        uint32_t pixel = diagonal ? gl_window_pixel(qts, i, i) :
                                    gl_window_pixel(qts, i, 8);

        g_assert_true(pixel == 0 || pixel == 0xffffff);
        drawn += pixel != 0;
    }
    return drawn;
}

static void test_gl_triangle_shared_edge_centres_drawn_once(void)
{
    static const double below_right[3][2] = {
        { 0, 0 }, { 16, 0 }, { 16, 16 },
    };
    static const double above_left[3][2] = {
        { 0, 0 }, { 16, 16 }, { 0, 16 },
    };
    static const double upper[3][2] = {
        { 0, 8.5 }, { 16, 8.5 }, { 8, 16 },
    };
    static const double lower[3][2] = {
        { 16, 8.5 }, { 0, 8.5 }, { 8, 1 },
    };

    g_assert_cmpuint(gl_count_drawn(qts, below_right, true), ==, 16);
    g_assert_cmpuint(gl_count_drawn(qts, above_left, true), ==, 0);
    g_assert_cmpuint(gl_count_drawn(qts, lower, false), ==, 16);
    g_assert_cmpuint(gl_count_drawn(qts, upper, false), ==, 0);
}

/*
 * The model bounds fragments to the viewport rectangle instead of clipping
 * to the view volume. A triangle reaching NDC -5 and 5 covers the whole
 * 64x64 viewport at (32, 32) and draws nothing outside it, though its
 * interior extends there.
 */
static void test_gl_triangle_bounded_by_the_viewport(void)
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 32, 32, 64, 64);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, -5, -5);
    gl_vertex2f(qts, 5, -5);
    gl_vertex2f(qts, 0, 5);
    gl_word(qts, GL_END_TRIANGLES);

    g_assert_cmphex(gl_window_pixel(qts, 32, 32), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 95, 32), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 32, 95), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 95, 95), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 31, 64), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 96, 64), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 31), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 96), ==, 0);
}

/*
 * A vertex with w <= 0 needs view-volume clipping, which the model does
 * not do, so it drops the triangle. (0.5, -1, 0, -1) divides to the same
 * point as (-0.5, 1, 0, 1), which draws.
 */
static void gl_draw_w_triangle(QTestState *qts, float x, float y, float w)
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex4f(qts, -1, -1, 0, 1);
    gl_vertex4f(qts, 1, -1, 0, 1);
    gl_vertex4f(qts, x, y, 0, w);
    gl_word(qts, GL_END_TRIANGLES);
}

static void test_gl_triangle_with_negative_w_is_dropped(void)
{
    gl_draw_w_triangle(qts, -0.5, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 20, 20), ==, 0xffffff);

    gl_draw_w_triangle(qts, 0.5, -1, -1);
    g_assert_cmphex(gl_window_pixel(qts, 20, 20), ==, 0);
}

/*
 * The GL client's matrix, depth and buffer words as the captured probe
 * streams of run irix-20260927T172059-fdbada carry them (glMatrixMode
 * 0x2a04, glLoadIdentity 0x2c00, glLoadMatrixf 0x2b40 with 16 floats in
 * memory order, glMultMatrixf 0x2d40, glFrustum 0x3418 and glOrtho 0x3518
 * with the six arguments as floats, glEnable(GL_DEPTH_TEST) 0x6604 with 1,
 * glDepthFunc 0x4304 with func - 0x200, glClearDepth 0xbc04 with the
 * 24-bit value, glClear's depth token 0x4a00, glDepthMask 0x3c04, the
 * draw-buffer triple 0x490c and the kernel's bank token 0x9808).
 */
#define GL_MATRIX_MODE_HDR    0x00002a04
#define GL_LOAD_MATRIX_HDR    0x00002b40
#define GL_LOAD_IDENTITY      0x00002c00
#define GL_MULT_MATRIX_HDR    0x00002d40
#define GL_FRUSTUM_HDR        0x00003418
#define GL_ORTHO_HDR          0x00003518
#define GL_DEPTH_TEST_HDR     0x00006604
#define GL_DEPTH_FUNC_HDR     0x00004304
#define GL_DEPTH_MASK_HDR     0x00003c04
#define GL_CLEAR_DEPTH_HDR    0x0000bc04
#define GL_CLEAR_DEPTH_BUFFER 0x00004a00
#define GL_DRAW_BUFFER_HDR    0x0000490c
#define GL_VERTEX3F_HDR       0x2000000c
#define GL_SWAP               0x00003700
#define GL_BANKS_HDR          0x00009808
#define GL_MODELVIEW          0x1700
#define GL_PROJECTION         0x1701
#define GL_TEXTURE            0x1702
#define GL_SMOOTH             0x1d01
#define GL_DEPTH_LESS         1
#define GL_DEPTH_NEVER        0
#define GL_DEPTH_EQUAL        2
#define GL_DEPTH_GREATER      4
#define GL_DEPTH_ALWAYS       7
#define GL_DEPTH_ONE          0xffffff

static void gl_vertex3f(QTestState *qts, float x, float y, float z)
{
    gl_word(qts, GL_VERTEX3F_HDR);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
}

static void gl_matrix(QTestState *qts, uint32_t header, const float m[16])
{
    gl_word(qts, header);
    for (int i = 0; i < 16; i++) {
        gl_float(qts, m[i]);
    }
}

static void gl_six(QTestState *qts, uint32_t header, float l, float r,
                   float b, float t, float n, float f)
{
    gl_word(qts, header);
    gl_float(qts, l);
    gl_float(qts, r);
    gl_float(qts, b);
    gl_float(qts, t);
    gl_float(qts, n);
    gl_float(qts, f);
}

static void gl_one(QTestState *qts, uint32_t header, uint32_t value)
{
    gl_word(qts, header);
    gl_word(qts, value);
}

static void gl_draw_buffer(QTestState *qts, uint32_t buffers)
{
    gl_word(qts, GL_DRAW_BUFFER_HDR);
    gl_word(qts, buffers);
    gl_word(qts, 1);
    gl_word(qts, 0);
}

/*
 * The kernel's MgrasValidateBanks as the swap captures show it: a 64-bit
 * store of the header and the first word, then a 32-bit store of the
 * second (mgras-gl-commands.md, "How the client reaches the port").
 */
static void gl_banks(QTestState *qts, uint32_t bank)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)GL_BANKS_HDR << 32) | bank);
    gl_word(qts, bank);
}

/*
 * One swap as the kernel performs it in the captured swap case: the bank
 * token, the swap token, then, once the swap interrupt is served, the main
 * buffer-select mask at XMAP index 0 (0x4000 after the first swap of the
 * window with display ID 14, 0 after the second).
 */
static void gl_swap(QTestState *qts, uint32_t bank, uint32_t mask)
{
    gl_banks(qts, bank);
    gl_word(qts, GL_SWAP);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, mask);
}

/* The window's pixels, top row first, as the expected PPM stores them. */
static void gl_check_window_sha256(const uint32_t *rgb, const char *expected)
{
    g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);

    for (int row = 0; row < 128; row++) {
        for (int col = 0; col < 128; col++) {
            uint32_t p = rgb[(256 + row) * 1280 + 256 + col];
            uint8_t bytes[3] = { p >> 16, p >> 8, p };

            g_checksum_update(sum, bytes, sizeof(bytes));
        }
    }
    g_assert_cmpstr(g_checksum_get_string(sum), ==, expected);
}

/* Screen pixel of window pixel (x, y), y counted up from the bottom. */
static uint32_t gl_dump_pixel(const uint32_t *rgb, int x, int y)
{
    return rgb[(383 - y) * 1280 + 256 + x];
}

/* The probe's window, display on, and the state every scene case sets. */
static void gl_scene_start(QTestState *qts, uint32_t shade)
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_one(qts, GL_SHADE_MODEL_HDR, shade);
}

/*
 * Window vertices (6, 98), (118, 54) and (38, 10) of the probe's ortho,
 * swap and swap-pending cases, as normalized device coordinates.
 */
static void gl_ortho_triangle_ndc(QTestState *qts)
{
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, -0.90625f, 0.53125f, 0);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 0.84375f, -0.15625f, 0);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, -0.40625f, -0.84375f, 0);
    gl_word(qts, GL_END_TRIANGLES);
}

/*
 * SHA-256 of the 128x128 expected windows gl_min_expected.py writes for
 * the probe's scene cases (Origami branch gl-probe-transform-depth),
 * computed from the OpenGL 1.1 rules and matched by Mesa llvmpipe, never
 * from this model.
 */
#define GL_ORTHO_SHA256 \
    "72929655985993b8505b211a3479c5841a5f93ab625cef6d0b32f7323abe99d8"
#define GL_FRUSTUM_SHA256 \
    "2411a9c279313dc62c8e536db291ba6d66abab75883d077aa70f92f127ed2746"
#define GL_DEPTH_SHA256 \
    "92a05b9a5c7c93ddee9a6c0a7717dff6b9983c968c4229e05bb04732fff2dbfe"
#define GL_SMOOTH_SHA256 \
    "77ae4e7831ad45733d6a4ac20c2ed0bfc030e5f321a55d496d0028638b8b1851"

/*
 * The ortho case: glOrtho(0, 128, 0, 128, -1, 1) on the projection, then
 * glLoadMatrixf of a modelview that maps object (x, y) to eye
 * (120 - 2y, x + 4). The window is the product projection x modelview
 * applied to each vertex (OpenGL 1.2.1, 2.10); applying them in the other
 * order, or reading the array by rows, moves the triangle off the window.
 */
static void test_gl_ortho_matches_the_reference(void)
{
    static const float modelview[16] = {
        0, 1, 0, 0, -2, 0, 0, 0, 0, 0, 1, 0, 120, 4, 0, 1,
    };
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_six(qts, GL_ORTHO_HDR, 0, 128, 0, 128, -1, 1);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, modelview);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 94, 57, 0);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 50, 1, 0);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 6, 41, 0);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 64, 64), ==, 0xffff00);
    g_assert_cmphex(gl_dump_pixel(rgb, 20, 20), ==, 0x0000ff);
    gl_check_window_sha256(rgb, GL_ORTHO_SHA256);
}

/*
 * The frustum case: glFrustum(-1, 1, -1, 1, 1, 8) after a modelview
 * translation of (0.25, -0.125, -3). The vertices land at eye depths 2, 4
 * and 3, so the divide by w places the third at window y 109 1/3.
 */
static void test_gl_frustum_matches_the_reference(void)
{
    static const float modelview[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.25f, -0.125f, -3, 1,
    };
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_six(qts, GL_FRUSTUM_HDR, -1, 1, -1, 1, 1, 8);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, modelview);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, -1.875f, -1.375f, 1);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 3.25f, -0.375f, -1);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 0.125f, 2.25f, 0);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 20, 20), ==, 0xffff00);
    g_assert_cmphex(gl_dump_pixel(rgb, 40, 60), ==, 0x0000ff);
    gl_check_window_sha256(rgb, GL_FRUSTUM_SHA256);
}

/*
 * glLoadMatrixf of a translation of +0.5 in x moves a triangle 32 pixels
 * right, and glMultMatrixf of it onto the identity does the same.
 */
static void gl_translated_triangle(QTestState *qts, uint32_t header)
{
    static const float shift[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1,
    };

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_matrix(qts, header, shift);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0);
    gl_vertex3f(qts, 0, -1, 0);
    gl_vertex3f(qts, -1, 0, 0);
    gl_word(qts, GL_END_TRIANGLES);
}

static void test_gl_load_matrix_reads_columns(void)
{
    /* The triangle's right-angle corner moves from (0, 0) to (32, 0). */
    gl_translated_triangle(qts, GL_LOAD_MATRIX_HDR);
    g_assert_cmphex(gl_window_pixel(qts, 33, 1), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);
}

/*
 * glMultMatrixf multiplies the current matrix on the right (OpenGL 1.2.1,
 * 2.10.2), so the argument applies to vertices first. A scale by 0.5
 * multiplied by a translation of +0.5 in x maps x to 0.5 (x + 0.5): the
 * triangle's corner at x -1 lands at window x 48. The other order,
 * 0.5 x + 0.5, would put it at 64.
 */
static void test_gl_mult_matrix_multiplies_on_the_right(void)
{
    static const float half[16] = {
        0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
    };
    static const float shift[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1,
    };

    gl_translated_triangle(qts, GL_MULT_MATRIX_HDR);
    g_assert_cmphex(gl_window_pixel(qts, 33, 1), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, half);
    gl_matrix(qts, GL_MULT_MATRIX_HDR, shift);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0);
    gl_vertex3f(qts, 0, -1, 0);
    gl_vertex3f(qts, -1, 0, 0);
    gl_word(qts, GL_END_TRIANGLES);
    /* Corner at window (48, 32); the other order puts it at (64, 32). */
    g_assert_cmphex(gl_window_pixel(qts, 49, 33), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 47, 33), ==, 0);
}

/*
 * Matrix commands act on the matrix glMatrixMode selects. A texture-matrix
 * load, which the model does not use, must leave modelview alone.
 */
static void test_gl_matrix_mode_selects_the_matrix(void)
{
    static const float scale[16] = {
        4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1,
    };

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_TEXTURE);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, scale);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0);
    gl_vertex3f(qts, -0.75f, -1, 0);
    gl_vertex3f(qts, -1, -0.75f, 0);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 2, 2), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 20, 2), ==, 0);
}

/*
 * Approximated: matrix products are stored at single precision, the
 * precision the client sends; the GE's arithmetic is not published. With
 * a z scale a = 1 + 2^-12 loaded and then multiplied in, a * a =
 * 1 + 2^-11 + 2^-24 rounds to 1 + 2^-11 in single precision. At object z
 * 1887437 / 2^21 the two products give window depths of 15942041.45 and
 * 15942041.90 steps, which round to different 24-bit values; a second
 * layer at z 0.900439441 with the identity matrix lands on the first.
 */
static void test_gl_matrix_product_is_single_precision(void)
{
    const float a = 1.0f + 1.0f / 4096;
    const float scale[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, a, 0, 0, 0, 0, 1,
    };
    const float z = 1887437.0f / 2097152.0f;

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_ALWAYS);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, scale);
    gl_matrix(qts, GL_MULT_MATRIX_HDR, scale);
    gl_color3f(qts, 1, 0, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, z);
    gl_vertex3f(qts, 3, -1, z);
    gl_vertex3f(qts, -1, 3, z);
    gl_word(qts, GL_END_TRIANGLES);

    gl_word(qts, GL_LOAD_IDENTITY);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_EQUAL);
    gl_color3f(qts, 0, 1, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0.900439441204071f);
    gl_vertex3f(qts, 3, -1, 0.900439441204071f);
    gl_vertex3f(qts, -1, 3, 0.900439441204071f);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0x00ff00);
}

/*
 * Inferred: the client's context-setup opener 0x0600 starts a new
 * context, whose matrices and depth state are OpenGL's initial ones even
 * though the setup after it sends none. A projection and depth test left
 * by an earlier context must not shrink or hide the new context's
 * drawing: in the probe's capture, the depth case followed the frustum
 * case in the same X session.
 */
static void test_gl_context_setup_restores_initial_state(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_six(qts, GL_FRUSTUM_HDR, -1, 1, -1, 1, 1, 8);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_NEVER);

    gl_word(qts, 0x00000600);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0);
    gl_vertex3f(qts, 0, -1, 0);
    gl_vertex3f(qts, -1, 0, 0);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 60, 1), ==, 0xffffff);
}

/*
 * The depth case: a red triangle at depth 0, then a green one on the plane
 * z = x / 2, depth test LESS after clearing depth to 1.0. Where they
 * overlap green is nearer left of window x 64 and red to the right; with
 * no depth test green would cover all of the overlap.
 */
static void test_gl_depth_matches_the_reference(void)
{
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex3f(qts, -0.78125f, -0.71875f, 0);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex3f(qts, 0.71875f, -0.59375f, 0);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex3f(qts, -0.09375f, 0.78125f, 0);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, -0.90625f, 0.53125f, -0.453125f);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, 0.84375f, 0.40625f, 0.421875f);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, 0.0625f, -0.921875f, 0.03125f);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 40, 60), ==, 0x00ff00);
    g_assert_cmphex(gl_dump_pixel(rgb, 90, 50), ==, 0xff0000);
    gl_check_window_sha256(rgb, GL_DEPTH_SHA256);
}

/* A whole-window triangle at normalized depth -1 + j / 2^24. */
static void gl_depth_layer(QTestState *qts, int j, float r, float g)
{
    float z = -1.0f + (float)j / 16777216.0f;

    gl_color3f(qts, r, g, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, z);
    gl_vertex3f(qts, 3, -1, z);
    gl_vertex3f(qts, -1, 3, z);
    gl_word(qts, GL_END_TRIANGLES);
}

static uint32_t gl_depth_pair(QTestState *qts, int first, int second)
{
    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_depth_layer(qts, first, 1, 0);
    gl_depth_layer(qts, second, 0, 1);
    return gl_window_pixel(qts, 10, 10);
}

/*
 * Approximated: the depth plane holds 24 bits, the RE4's published Z
 * width (Octane Technical Report, 4.2.3), and a fragment's window depth
 * is rounded to the nearest of them; how the board rounds is not
 * published. At z = -1 + j / 2^24 the stored value is j / 2 - j / 2^25:
 * j = 4 rounds to 2 (truncation gives 1), j = 3 to 1, j = 2 to 1
 * (truncation gives 0, and unquantized depth keeps it below j = 3's).
 * So the second layer of (4, 3) passes LESS only under rounding, and the
 * second of (3, 2) fails only under rounding to 24 bits.
 */
static void test_gl_depth_rounds_to_24_bits(void)
{
    g_assert_cmphex(gl_depth_pair(qts, 4, 3), ==, 0x00ff00);
    g_assert_cmphex(gl_depth_pair(qts, 3, 2), ==, 0xff0000);
}

/*
 * With the depth test disabled the depth buffer is not updated (OpenGL
 * 1.2.1, 4.1.5), so a far triangle drawn after enabling it still passes
 * against the cleared 1.0. With the test on but the mask off, the same
 * holds (4.2.2).
 */
static void test_gl_depth_writes_only_when_tested_and_masked_in(void)
{
    for (int masked = 0; masked < 2; masked++) {
        gl_scene_start(qts, GL_FLAT);
        gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
        gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
        gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
        if (masked) {
            gl_one(qts, GL_DEPTH_TEST_HDR, 1);
            gl_one(qts, GL_DEPTH_MASK_HDR, 0);
        }
        gl_depth_layer(qts, 0, 1, 0);
        gl_one(qts, GL_DEPTH_TEST_HDR, 1);
        gl_one(qts, GL_DEPTH_MASK_HDR, 1);
        gl_depth_layer(qts, 1 << 23, 0, 1);
        g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0x00ff00);
    }
}

/*
 * glDepthFunc sends func - GL_NEVER, and each comparison follows 4.1.5:
 * NEVER draws nothing, ALWAYS draws over a nearer surface, GREATER draws
 * only a farther one.
 */
static void test_gl_depth_func_follows_the_enum(void)
{
    static const struct {
        uint32_t func;
        int j;
        uint32_t expected;
    } cases[] = {
        { GL_DEPTH_NEVER, 0, 0xff0000 },
        { GL_DEPTH_ALWAYS, 1 << 24, 0x00ff00 },
        { GL_DEPTH_GREATER, 1 << 22, 0xff0000 },
        { GL_DEPTH_GREATER, 1 << 24, 0x00ff00 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        gl_scene_start(qts, GL_FLAT);
        gl_one(qts, GL_DEPTH_TEST_HDR, 1);
        gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_ALWAYS);
        gl_depth_layer(qts, 1 << 23, 1, 0);
        gl_one(qts, GL_DEPTH_FUNC_HDR, cases[i].func);
        gl_depth_layer(qts, cases[i].j, 0, 1);
        g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, cases[i].expected);
    }
}

/*
 * glClear's depth token fills the window's depth with the glClearDepth
 * value: cleared to 0, nothing passes LESS; cleared to 2^24 - 1, the same
 * triangle draws. Pixels outside the window keep their depth.
 */
static void test_gl_clear_depth_sets_the_plane(void)
{
    for (int far = 0; far < 2; far++) {
        gl_scene_start(qts, GL_FLAT);
        gl_one(qts, GL_DEPTH_TEST_HDR, 1);
        gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
        gl_one(qts, GL_CLEAR_DEPTH_HDR, far ? GL_DEPTH_ONE : 0);
        gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
        gl_depth_layer(qts, 1 << 23, 0, 1);
        g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==,
                        far ? 0x00ff00 : 0);
    }
}

/*
 * A fragment whose window depth lies outside [0, 1] belongs to the part
 * of the triangle beyond the far or near plane, which clipping removes
 * (OpenGL 1.2.1, 2.11). Window depth here is 0.25 + 1.5 * y / 128, so
 * rows from 64 up are beyond the far plane, depth test or not.
 */
static void test_gl_triangle_clipped_at_the_far_plane(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, -0.5f);
    gl_vertex3f(qts, 1, -1, -0.5f);
    gl_vertex3f(qts, -1, 1, 2.5f);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 2, 63), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 2, 64), ==, 0);
}

/*
 * The smooth case: red, green and blue vertices at window (0.4375,
 * 0.4375), (127.9375, 0.4375) and (64.1875, 127.9375). Every channel of
 * every pixel is at least 1/32 of a step from a rounding tie, so any
 * sound interpolation gives these 8,192 colours.
 */
static void test_gl_smooth_matches_the_reference(void)
{
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_SMOOTH);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex3f(qts, -0.9931640625f, -0.9931640625f, 0);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, 0.9990234375f, -0.9931640625f, 0);
    gl_color3f(qts, 0, 0, 1);
    gl_vertex3f(qts, 0.0029296875f, 0.9990234375f, 0);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    /* Worked by hand: blue 126.125, green 63.0625, red 65.8125. */
    g_assert_cmphex(gl_dump_pixel(rgb, 63, 63), ==, 0x423f7e);
    g_assert_cmphex(gl_dump_pixel(rgb, 0, 0), ==, 0xff0000);
    gl_check_window_sha256(rgb, GL_SMOOTH_SHA256);
}

/*
 * Approximated: an interpolated colour is stored at eight bits a channel,
 * rounded to nearest (OpenGL 1.2.1, 2.13.9); the RE4 iterates twelve bits
 * and how the board reduces them is not published. Red runs from 0 at
 * window x 0 to 1 at x 128 along the bottom row, so pixel 0's centre has
 * 255 * 0.5 / 128 = 0.996 (1, truncation 0) and pixel 1's 2.988 (3,
 * truncation 2).
 */
static void test_gl_smooth_rounds_to_the_nearest_step(void)
{
    gl_scene_start(qts, GL_SMOOTH);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 0, 0, 1);
    gl_vertex3f(qts, -1, -1, 0);
    gl_color3f(qts, 1, 0, 1);
    gl_vertex3f(qts, 1, -1, 0);
    gl_color3f(qts, 0, 0, 1);
    gl_vertex3f(qts, -1, 1, 0);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 0, 0) >> 16, ==, 1);
    g_assert_cmphex(gl_window_pixel(qts, 1, 0) >> 16, ==, 3);
}

/*
 * Colour is interpolated with the perspective correction of OpenGL 1.2.1,
 * 3.5.1 (equation 3.4). The third vertex (-2, 2, 0, 2) projects to window
 * (0, 128) with red 1 and the others are black at w 1. At window pixel
 * (0, 31) the screen-space weight of the third vertex is 31.5 / 128, so
 * linear interpolation gives red 62.75 (63); corrected, the weight is
 * (b / 2) / (1 - b / 2) = 0.1403, red 35.78 (36).
 */
static void test_gl_smooth_is_perspective_correct(void)
{
    gl_scene_start(qts, GL_SMOOTH);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 0, 0, 0);
    gl_vertex4f(qts, -1, -1, 0, 1);
    gl_color3f(qts, 0, 0, 0);
    gl_vertex4f(qts, 1, -1, 0, 1);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex4f(qts, -2, 2, 0, 2);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 0, 31), ==, 0x240000);
}

/*
 * Inferred from the captures: the draw-buffer triple's first word is 1 for
 * the single-buffered visual and 2 for the double-buffered one, the
 * front and back buffer; context-image words 16 and 17 are 1 in every GL
 * image and the kernel's bank token carries 0 then 1 at the two swaps, so
 * those words name the bank that holds the back buffer and the front is
 * the other. Bank 0 is the one the X server draws in. A back-buffer clear
 * therefore leaves bank 0 alone and shows only where the XMAP main
 * buffer-select mask selects bank 1 for the pixel's display ID.
 */
static void test_gl_back_buffer_draws_into_bank_one(void)
{
    gl_scene_start(qts, GL_FLAT);
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    gl_draw_buffer(qts, 2);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);

    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0xff00ff);
    /* Display ID 0 around the window still shows bank 0. */
    g_assert_cmphex(mgras_test_display_pixel(qts, 250, 300), ==, 0);
}

/* The front buffer is the bank the back is not. */
static void test_gl_front_buffer_is_the_other_bank(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_draw_buffer(qts, 1);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0x00ff00);

    gl_banks(qts, 0);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0x00ff00);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0xff0000);
}

/*
 * The bank token moves the back buffer: after (0, 0), which the kernel
 * sends at a window's first swap, back-buffer drawing lands in bank 0.
 */
static void test_gl_bank_token_moves_the_back_buffer(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_draw_buffer(qts, 2);
    gl_banks(qts, 0);
    gl_clear_color(qts, 0, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0x00ffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Inferred: a context switch restores the bank words from image words 16
 * and 17. An image carrying 0 puts the back buffer in bank 0.
 */
static void test_gl_context_image_restores_the_banks(void)
{
    uint32_t image[MGRAS_CONTEXT_WORDS];

    gl_scene_start(qts, GL_FLAT);
    gl_banks(qts, 0);
    memcpy(image, gl_context_image, sizeof(image));
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, image[i]);
    }
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
    gl_draw_buffer(qts, 2);
    gl_clear_color(qts, 1, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0);
}

/*
 * The captured swap case: the stale magenta frame, a swap that selects
 * bank 1 for display ID 14, the scene into bank 0, a swap back. The
 * window shows the scene, not the stale frame.
 */
static void test_gl_swap_matches_the_reference(void)
{
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    gl_draw_buffer(qts, 2);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_swap(qts, 0, 1u << 14);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_ortho_triangle_ndc(qts);
    gl_swap(qts, 1, 0);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 64, 64), ==, 0xffff00);
    gl_check_window_sha256(rgb, GL_ORTHO_SHA256);
}

/*
 * The captured swap-pending case: the scene, a swap, then the stale frame
 * into the new back buffer with no swap. The stale frame must not show.
 */
static void test_gl_swap_pending_matches_the_reference(void)
{
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    gl_draw_buffer(qts, 2);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_ortho_triangle_ndc(qts);
    gl_swap(qts, 0, 1u << 14);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 64, 64), ==, 0xffff00);
    gl_check_window_sha256(rgb, GL_ORTHO_SHA256);
}

/*
 * Context image word 0 names the context's GL state: 0x3bcd and 0x5420
 * for the two probe clients of run irix-20260927T191513-b4997c, which
 * never re-send their state after a switch. The tests give both contexts
 * the same window, so a model that keyed state by the window, or kept one
 * state for the board, fails them.
 */
#define GL_SLOT_A             0x3bcd
#define GL_SLOT_B             0x5420
#define GL_SLOT_DEFAULT       0xb27
#define GL_CONTEXT_OPENER     0x00000600

static void gl_enter_slot(QTestState *qts, uint32_t slot)
{
    mgras_enter_gl_slot(qts, slot, 256, 640, 128, 128, 0x11);
}

/* glOrtho(0, 128, 0, 128, -1, 1): vertices in window coordinates. */
static void gl_window_projection(QTestState *qts)
{
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_six(qts, GL_ORTHO_HDR, 0, 128, 0, 128, -1, 1);
}

/*
 * Draw-buffer word 3 selects the front and the back buffer together, so a
 * clear and a triangle each reach both banks: bank 0 directly, and bank 1
 * where the XMAP buffer-select mask shows it for the window's display ID.
 */
static void test_gl_front_and_back_draw_into_both_banks(void)
{
    gl_scene_start(qts, GL_FLAT);
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_draw_buffer(qts, 3);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 10.25f, 10.25f);
    gl_vertex2f(qts, 10.875f, 10.25f);
    gl_vertex2f(qts, 10.25f, 10.875f);
    gl_word(qts, GL_END_TRIANGLES);

    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0xffffff);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 10, 383 - 10), ==,
                    0xffffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Client a's setup in the alternate case, then client b's, which sets a
 * smaller viewport, another clear and current colour, the identity
 * projection, a depth test that passes nothing and the back buffer. Back
 * in a, a clear and a triangle use a's state; back in b, b's.
 */
static void test_gl_state_follows_the_context_slot(void)
{
    mgras_reset();
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_color3f(qts, 1, 1, 0);
    gl_window_projection(qts);
    gl_draw_buffer(qts, 1);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 64, 64);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_color3f(qts, 1, 0, 1);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_NEVER);
    gl_draw_buffer(qts, 2);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 16, 16);
    gl_vertex2f(qts, 112, 24);
    gl_vertex2f(qts, 56, 111);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 61, 50), ==, 0xffff00);
    g_assert_cmphex(gl_window_pixel(qts, 120, 120), ==, 0xff0000);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 120, 120), ==, 0xff0000);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0x00ff00);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The opener starts only the context it is sent in: b's opener returns b
 * to the initial clear colour and leaves a's alone.
 */
static void test_gl_context_opener_resets_only_its_context(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 1, 0, 0, 1);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x00ff00);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff0000);
}

/*
 * The kernel writes -1 in word 0 for a context with no GL state, and its
 * default context carries 0xb27 in the captures. Both select the one
 * state kept for no context, which a GL context's drawing leaves alone.
 */
static void test_gl_images_without_a_slot_share_one_state(void)
{
    mgras_reset();

    gl_enter_slot(qts, 0xffffffff);
    gl_clear_color(qts, 0, 0, 1, 1);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff0000);

    gl_enter_slot(qts, GL_SLOT_DEFAULT);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x0000ff);
}

/*
 * The model holds 32 slots. A 33rd replaces the one selected least
 * recently, and a replaced slot selected again starts from OpenGL's
 * initial state. Selecting slot 0 again after filling the table makes
 * slot 1 the least recent, so a model that replaced the oldest entry
 * instead, or held more than 32, would keep slot 1's green.
 */
static void test_gl_context_table_replaces_the_least_recent(void)
{
    mgras_reset();

    for (int i = 0; i < 32; i++) {
        gl_enter_slot(qts, 0x1000 + i);
        gl_word(qts, GL_CONTEXT_OPENER);
        gl_clear_color(qts, i == 0, i == 1, i > 1, 1);
    }
    gl_enter_slot(qts, 0x1000);
    gl_enter_slot(qts, 0x1000 + 32);
    gl_enter_slot(qts, 0x1000);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff0000);

    gl_enter_slot(qts, 0x1001);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A switch can land between the vertices of a triangle: the client does
 * not know it happened. a's first two vertices wait in a's state while b
 * draws a whole triangle of its own, and a's third completes a's.
 */
static void test_gl_triangle_spans_a_context_switch(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 0);
    gl_window_projection(qts);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 16, 16);
    gl_vertex2f(qts, 112, 24);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 0, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 0.5f, 0.5f);
    gl_vertex2f(qts, 0.9f, 0.5f);
    gl_vertex2f(qts, 0.5f, 0.9f);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 100, 100), ==, 0xff00ff);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_vertex2f(qts, 56, 111);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 61, 50), ==, 0xffff00);
}

/*
 * A switch can land inside a packet. In run irix-20260928T000530-78d0c5
 * (seq 12929748 to 12930582) perfly's context 0x84c6 was switched out 33
 * words into the 156 data words of its fourth GE table load (header
 * 0x80000284), context 0x5420 then sent one glFlush, and 0x84c6 came back
 * and sent the rest of its block. Each context's words are its own
 * stream: b's clear must draw, and a's remaining block words, fence tokens
 * here, must stay data.
 */
static void test_gl_parser_follows_the_context(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, 0x80000284);
    for (int i = 0; i < 5 + 33; i++) {
        gl_word(qts, MGRAS_FENCE_TOKEN);
    }

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x00ff00);

    gl_enter_slot(qts, GL_SLOT_A);
    for (int i = 33; i < 156; i++) {
        gl_word(qts, MGRAS_FENCE_TOKEN);
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff0000);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

/*
 * The payload words a context has already sent wait with it: a's
 * glClearColor is split by b's whole clear, and a's colour is the four
 * words a sent, not b's.
 */
static void test_gl_packet_split_by_a_context_switch(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_HDR);
    gl_float(qts, 1);
    gl_float(qts, 0);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 0, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x00ffff);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_float(qts, 1);
    gl_float(qts, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff00ff);
}

/*
 * glColor3ub, glColor4ub and glVertex2i (mgras-gl-commands.md, vertex
 * attributes): 0x34800203 and one word, 0x14800204 and one word,
 * 0x58000008 and two ints.
 */
#define GL_COLOR3UB_HDR       0x34800203
#define GL_COLOR4UB_HDR       0x14800204
#define GL_VERTEX2I_HDR       0x58000008

/* A flat triangle over window pixel (10, 10) in the current colour. */
static void gl_corner_triangle(QTestState *qts)
{
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, -1, -1);
    gl_vertex2f(qts, 1, -1);
    gl_vertex2f(qts, -1, 1);
    gl_word(qts, GL_END_TRIANGLES);
}

/*
 * colorbars (run irix-20260928T004550-1040f3, from seq 26371573) sends its
 * bars' colours as glColor3ub words 0x87878700, 0x87870000, 0x00878700,
 * 0x00870000, 0x87008700 and 0x87000000: white, yellow, cyan, green,
 * magenta and red at 0x87, so the three bytes arrive red first from bit
 * 31. A byte converts to byte / 255 (OpenGL 1.2.1, table 2.6), so 0x80
 * stays 0x80 in an eight-bit channel. The fourth byte of glColor4ub is
 * alpha, which follows from that order; noof's constant 0x00000060 is
 * consistent with it.
 */
static void test_gl_color_bytes_unpack_red_first(void)
{
    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_one(qts, GL_COLOR3UB_HDR, 0xff800100);
    gl_corner_triangle(qts);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0xff8001);

    gl_one(qts, GL_COLOR4UB_HDR, 0x2040c0ff);
    gl_corner_triangle(qts);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0x2040c0);

    /* The one payload word was consumed: the next word is a header. */
    gl_one(qts, GL_COLOR3UB_HDR, 0x00ff0000);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    gl_corner_triangle(qts);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0x00ff00);
}

/*
 * The probe's triangle, sent as glVertex2i window units through
 * glOrtho(0, 128, 0, 128, -1, 1), draws the triangle case's pixels.
 */
static void test_gl_vertex2i_matches_the_reference(void)
{
    static const int32_t v[3][2] = { { 16, 16 }, { 112, 24 }, { 56, 111 } };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_color3f(qts, 1, 1, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        gl_word(qts, GL_VERTEX2I_HDR);
        gl_word(qts, v[i][0]);
        gl_word(qts, v[i][1]);
    }
    gl_word(qts, GL_END_TRIANGLES);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
}

/*
 * The matrix stack and transform commands, named by the client's own
 * display-list decoder (notes-lists-arrays-lighting.md, "The CPU decoder
 * as a second opcode table"): glPushMatrix 0x2f00, glPopMatrix 0x2e00,
 * glRotatef 0x3010 (angle in degrees, x, y, z), glScalef 0x310c and
 * glTranslatef 0x320c.
 */
#define GL_PUSH_MATRIX        0x00002f00
#define GL_POP_MATRIX         0x00002e00
#define GL_ROTATE_HDR         0x00003010
#define GL_SCALE_HDR          0x0000310c
#define GL_TRANSLATE_HDR      0x0000320c

static void gl_three(QTestState *qts, uint32_t header, float x, float y,
                     float z)
{
    gl_word(qts, header);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
}

static void gl_rotate(QTestState *qts, float angle, float x, float y,
                      float z)
{
    gl_word(qts, GL_ROTATE_HDR);
    gl_float(qts, angle);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
}

/* A window-unit viewport, glOrtho(0, 128, 0, 128, -1, 1), modelview mode. */
static void gl_transform_start(QTestState *qts)
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
}

/*
 * A small white triangle covering only the pixel whose lower-left corner
 * the modelview maps object (x, y) to, after clearing the window black.
 */
static void gl_mark(QTestState *qts, float x, float y)
{
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, x + 0.25f, y + 0.25f);
    gl_vertex2f(qts, x + 0.875f, y + 0.25f);
    gl_vertex2f(qts, x + 0.25f, y + 0.875f);
    gl_word(qts, GL_END_TRIANGLES);
}

/*
 * glTranslatef, glRotatef and glScalef multiply the current matrix on the
 * right (OpenGL 1.2.1, 2.10.2), so the last one sent applies to the
 * vertex first. The rotation is counter-clockwise looking down the axis
 * toward the origin, by an angle in degrees, about the axis made unit
 * length. Here object (x, y) goes to window (64 - y / 2, 32 + 2x): the
 * triangle below lands around pixel centre (64.5, 52.5) and on no other.
 */
static void test_gl_transforms_multiply_on_the_right(void)
{
    gl_transform_start(qts);
    gl_three(qts, GL_TRANSLATE_HDR, 64, 32, 0);
    gl_rotate(qts, 90, 0, 0, 2);
    gl_three(qts, GL_SCALE_HDR, 2, 0.5f, 1);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 10.125f, -0.5f);
    gl_vertex2f(qts, 10.125f, -1.75f);
    gl_vertex2f(qts, 10.4375f, -0.5f);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 64, 52), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 65, 52), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 63, 52), ==, 0);
}

/*
 * A half turn about (2, 2, 0), made unit length, swaps x and y and
 * negates z: a mark at (40, 10) lands on (10, 40).
 */
static void test_gl_rotate_about_a_general_axis(void)
{
    gl_transform_start(qts);
    gl_rotate(qts, 180, 2, 2, 0);
    gl_mark(qts, 40, 10);
    g_assert_cmphex(gl_window_pixel(qts, 10, 40), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 40, 10), ==, 0);
}

/*
 * glPushMatrix saves the current matrix of the current mode and
 * glPopMatrix restores it (2.10.2). The stacks are separate: a pop in
 * projection mode with nothing pushed there changes nothing, and the
 * modelview's saved matrix waits for a modelview pop.
 */
static void test_gl_push_and_pop_restore_the_matrix(void)
{
    gl_transform_start(qts);
    gl_three(qts, GL_TRANSLATE_HDR, 8, 0, 0);
    gl_word(qts, GL_PUSH_MATRIX);
    gl_three(qts, GL_TRANSLATE_HDR, 0, 16, 0);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_POP_MATRIX);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_mark(qts, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 9, 17), ==, 0xffffff);
    gl_word(qts, GL_POP_MATRIX);
    gl_mark(qts, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 9, 1), ==, 0xffffff);

    /* A projection push and pop leave the projection as it was. */
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_PUSH_MATRIX);
    gl_three(qts, GL_TRANSLATE_HDR, 64, 0, 0);
    gl_word(qts, GL_POP_MATRIX);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_mark(qts, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 9, 1), ==, 0xffffff);
}

/*
 * Approximated: the board's stack depths are not known. The model holds
 * 32 matrices in each stack, current one included, the least OpenGL
 * 1.2.1 allows for the modelview stack (table 6.25). A push onto a full
 * stack and a pop of the last matrix change nothing, as the specification
 * has the stack-overflow and stack-underflow errors do. Load a
 * translation of 3k + 1 before the k-th push: after 31 pushes the stack
 * is full, the 32nd push is ignored, so a pop returns translation 91, not
 * 94; after the pops return to translation 1 a further pop leaves it.
 */
static void test_gl_matrix_stack_holds_32(void)
{
    gl_transform_start(qts);
    for (int k = 0; k < 32; k++) {
        gl_word(qts, GL_LOAD_IDENTITY);
        gl_three(qts, GL_TRANSLATE_HDR, 3 * k + 1, 0, 0);
        gl_word(qts, GL_PUSH_MATRIX);
    }
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_three(qts, GL_TRANSLATE_HDR, 120, 0, 0);
    gl_word(qts, GL_POP_MATRIX);
    gl_mark(qts, 0, 10);
    g_assert_cmphex(gl_window_pixel(qts, 91, 10), ==, 0xffffff);
    for (int k = 0; k < 30; k++) {
        gl_word(qts, GL_POP_MATRIX);
    }
    gl_mark(qts, 0, 10);
    g_assert_cmphex(gl_window_pixel(qts, 1, 10), ==, 0xffffff);
    gl_word(qts, GL_POP_MATRIX);
    gl_mark(qts, 0, 10);
    g_assert_cmphex(gl_window_pixel(qts, 1, 10), ==, 0xffffff);
}

/*
 * The probe's primitive, transform and format cases (Origami branch
 * mgras-probes-primitives, gl_min_probe.c's prim_cases), sent as the
 * IRIX 6.5.30 client frames them: glViewport, glDisable(GL_DITHER)
 * (0x6704 + 0), glShadeModel(GL_FLAT), a blue clear, glOrtho(0, 128, 0,
 * 128, -1, 1) on the projection, then the case's matrix commands and
 * primitives in order, then glFlush. Each window must match the image
 * gl_min_expected.py computes from the OpenGL 1.1 rules, which Mesa
 * llvmpipe matches exactly; the SHA-256 is of its 128x128 RGB bytes, top
 * row first, never of this model's output.
 */
enum {
    GL_PRIM_END, GL_PRIM_DRAW, GL_PRIM_PUSH, GL_PRIM_POP, GL_PRIM_TRANSLATE,
    GL_PRIM_ROTATE, GL_PRIM_SCALE,
};
enum {
    GL_PRIM_FLOAT, GL_PRIM_COLOR3UB, GL_PRIM_COLOR4UB, GL_PRIM_VERTEX2I,
};

typedef struct GlPrimOp {
    int op;
    float arg[4];
} GlPrimOp;

typedef struct GlPrimVertex {
    float x, y;
    uint8_t rgb[3];
} GlPrimVertex;

typedef struct GlPrimCase {
    const char *name;
    const char *sha256;
    uint32_t begin;
    int format;
    GlPrimOp op[6];
    int count;
    GlPrimVertex v[8];
} GlPrimCase;

#define GL_DITHER_HDR         0x00006704
/* glBegin's token; glEnd's is 0x0a00 above it (mgras-gl-commands.md). */
#define GL_BEGIN_POINTS       0x00001600
#define GL_BEGIN_LINES        0x00001700
#define GL_BEGIN_LINE_STRIP   0x00001800
#define GL_BEGIN_LINE_LOOP    0x00001900
#define GL_BEGIN_TRI_STRIP    0x00001b00
#define GL_BEGIN_TRI_FAN      0x00001c00
#define GL_BEGIN_QUADS        0x00001d00
#define GL_BEGIN_QUAD_STRIP   0x00001e00
#define GL_BEGIN_POLYGON      0x00001f00
#define GL_END_OF(begin)      ((begin) + 0x0a00)

static const GlPrimCase gl_prim_cases[] = {
    { "points",
      "482d539f231a1f18291197f2e286d821c3f1a50de839fd6ca2d06492bed83d1c",
      GL_BEGIN_POINTS, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 6,
      { { 10.25f, 10.75f, { 255, 0, 0 } }, { 20.5f, 100.5f, { 0, 255, 0 } },
        { 64.75f, 64.25f, { 255, 255, 0 } },
        { 100.125f, 30.875f, { 255, 255, 255 } },
        { 120.5f, 120.5f, { 255, 0, 255 } },
        { 5.5f, 60.25f, { 0, 255, 255 } } } },
    { "lines",
      "0d557e4410ce6f4690519f5133090dde422327cc709289730388558035cf0ef0",
      GL_BEGIN_LINES, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 8,
      { { 10.5f, 20.5f, { 255, 0, 0 } }, { 60.5f, 20.5f, { 255, 255, 0 } },
        { 30.25f, 40.875f, { 255, 0, 0 } },
        { 30.25f, 110.125f, { 0, 255, 0 } },
        { 15.25f, 30.125f, { 255, 0, 0 } },
        { 110.625f, 70.28125f, { 255, 255, 255 } },
        { 70.375f, 120.25f, { 255, 0, 0 } },
        { 90.53125f, 35.59375f, { 0, 255, 255 } } } },
    { "line-strip",
      "000abf8ff15b541f2dc2cefc10606cadad946b5ae9d47d15f8334271a602d7e7",
      GL_BEGIN_LINE_STRIP, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 5,
      { { 8.15625f, 9.40625f, { 255, 0, 0 } },
        { 62.15625f, 36.40625f, { 255, 255, 0 } },
        { 102.15625f, 6.40625f, { 0, 255, 0 } },
        { 117.15625f, 81.40625f, { 255, 255, 255 } },
        { 37.15625f, 111.40625f, { 255, 0, 255 } } } },
    { "line-loop",
      "c5aa30dcc66e8c98bdbf708e77ca5767ff3fc302f670db1d1034fd2d315c1cf6",
      GL_BEGIN_LINE_LOOP, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 4,
      { { 20.15625f, 20.75f, { 255, 0, 0 } },
        { 100.15625f, 30.75f, { 255, 255, 0 } },
        { 90.15625f, 100.75f, { 0, 255, 0 } },
        { 30.15625f, 90.75f, { 255, 255, 255 } } } },
    { "triangle-strip",
      "ba7ee78db90a8f00ea0a6df54de7578922e0021e3b05c78afe84388320e27cae",
      GL_BEGIN_TRI_STRIP, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 6,
      { { 10.09375f, 20.34375f, { 0, 0, 0 } },
        { 20.09375f, 90.34375f, { 0, 0, 0 } },
        { 45.09375f, 15.34375f, { 255, 0, 0 } },
        { 55.09375f, 100.34375f, { 255, 255, 0 } },
        { 80.09375f, 25.34375f, { 0, 255, 0 } },
        { 100.09375f, 95.34375f, { 255, 255, 255 } } } },
    { "triangle-fan",
      "ae2e1ba0f39fa9efb41d2b459b71a96e55e46beb218834471d3fa0011f97cc20",
      GL_BEGIN_TRI_FAN, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 5,
      { { 64.09375f, 60.53125f, { 0, 0, 0 } },
        { 109.09375f, 45.53125f, { 0, 0, 0 } },
        { 100.09375f, 108.53125f, { 255, 0, 0 } },
        { 40.09375f, 116.53125f, { 255, 255, 0 } },
        { 16.09375f, 72.53125f, { 0, 255, 0 } } } },
    { "quads",
      "6d371b52adc58049677e317f13f4e40e2522998eec59a45f073ef914e304e11d",
      GL_BEGIN_QUADS, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 8,
      { { 10.03125f, 10.53125f, { 255, 0, 0 } },
        { 60.03125f, 15.53125f, { 255, 0, 0 } },
        { 55.03125f, 60.53125f, { 255, 0, 0 } },
        { 15.03125f, 55.53125f, { 255, 255, 0 } },
        { 70.03125f, 70.53125f, { 255, 0, 0 } },
        { 120.03125f, 75.53125f, { 255, 0, 0 } },
        { 115.03125f, 120.53125f, { 255, 0, 0 } },
        { 65.03125f, 110.53125f, { 0, 255, 0 } } } },
    { "quad-strip",
      "6a48120877e2d60a694003b7c6ff9a9f96e4c65bd12e098a8ee9200edb7fb64b",
      GL_BEGIN_QUAD_STRIP, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 6,
      { { 10.09375f, 20.15625f, { 255, 0, 0 } },
        { 10.09375f, 100.15625f, { 255, 0, 0 } },
        { 60.09375f, 10.15625f, { 255, 0, 0 } },
        { 55.09375f, 110.15625f, { 255, 255, 0 } },
        { 120.09375f, 30.15625f, { 255, 0, 0 } },
        { 110.09375f, 90.15625f, { 0, 255, 0 } } } },
    { "polygon",
      "fee3202432cb5881ba63abdd142b7007e54df5b5de6480b9953177da61a69731",
      GL_BEGIN_POLYGON, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 6,
      { { 64.125f, 8.40625f, { 255, 255, 0 } },
        { 112.125f, 36.40625f, { 255, 0, 0 } },
        { 108.125f, 92.40625f, { 0, 255, 0 } },
        { 60.125f, 120.40625f, { 255, 255, 255 } },
        { 16.125f, 92.40625f, { 255, 0, 255 } },
        { 20.125f, 36.40625f, { 0, 255, 255 } } } },
    { "push-pop",
      "e5b12c008b2c481baf89277d4c29f7ac858fa1c4baee90be25f6ad8b78dc63e2",
      GL_BEGIN_TRIANGLES, GL_PRIM_FLOAT,
      { { GL_PRIM_PUSH }, { GL_PRIM_TRANSLATE, { 60, 50, 0 } },
        { GL_PRIM_DRAW }, { GL_PRIM_POP }, { GL_PRIM_DRAW } }, 3,
      { { 10.03125f, 10.09375f, { 255, 255, 0 } },
        { 50.03125f, 20.09375f, { 255, 255, 0 } },
        { 20.03125f, 55.09375f, { 255, 255, 0 } } } },
    { "rotate",
      "8b3d9083f192fb194c6cfe36d1a550caa2cc9829181d68149f0e9af144cdb704",
      GL_BEGIN_TRIANGLES, GL_PRIM_FLOAT,
      { { GL_PRIM_TRANSLATE, { 64.5625f, 60.5f, 0 } },
        { GL_PRIM_ROTATE, { 30, 0, 0, 2 } }, { GL_PRIM_DRAW } }, 3,
      { { -20, -10, { 255, 255, 0 } }, { 25, -15, { 255, 255, 0 } },
        { 0, 30, { 255, 255, 0 } } } },
    { "scale",
      "91af7b3f7722ad9aaf7e1987b8fb96d9dba646f95144fc90c9ac2c7bbf9ace8a",
      GL_BEGIN_TRIANGLES, GL_PRIM_FLOAT,
      { { GL_PRIM_TRANSLATE, { 10, 15.625f, 0 } },
        { GL_PRIM_SCALE, { 1.5f, 0.75f, 1 } }, { GL_PRIM_DRAW } }, 3,
      { { 0, 0, { 255, 255, 0 } }, { 70, 10, { 255, 255, 0 } },
        { 20, 120, { 255, 255, 0 } } } },
    { "color3ub",
      "d09bf68e85017e80d767dd08a9495b9236a1d0e3cd023205ff1f26745b51e31b",
      GL_BEGIN_TRIANGLES, GL_PRIM_COLOR3UB, { { GL_PRIM_DRAW } }, 6,
      { { 10, 10.3125f, { 0, 0, 0 } }, { 60, 20.3125f, { 0, 0, 0 } },
        { 20, 60.3125f, { 255, 128, 1 } }, { 70, 60.3125f, { 0, 0, 0 } },
        { 120, 70.3125f, { 0, 0, 0 } }, { 80, 120.3125f, { 64, 192, 32 } } } },
    { "color4ub",
      "c04cac041e808b0c99e96cf4e3611512b18bdaac742cb6ad0934fda0058e5929",
      GL_BEGIN_TRIANGLES, GL_PRIM_COLOR4UB, { { GL_PRIM_DRAW } }, 6,
      { { 10, 10.3125f, { 0, 0, 0 } }, { 60, 20.3125f, { 0, 0, 0 } },
        { 20, 60.3125f, { 32, 64, 192 } }, { 70, 60.3125f, { 0, 0, 0 } },
        { 120, 70.3125f, { 0, 0, 0 } },
        { 80, 120.3125f, { 200, 100, 50 } } } },
    { "vertex2i",
      "d18b98ea09eb97c0803ac05d5c8fcb55d329817c833e3d3cd2679a355ecadf0f",
      GL_BEGIN_TRIANGLES, GL_PRIM_VERTEX2I, { { GL_PRIM_DRAW } }, 3,
      { { 30, 40, { 255, 255, 0 } }, { 70, 50, { 255, 255, 0 } },
        { 40, 85, { 255, 255, 0 } } } },
};

static void gl_prim_draw(QTestState *qts, const GlPrimCase *c)
{
    gl_word(qts, c->begin);
    for (int i = 0; i < c->count; i++) {
        const GlPrimVertex *v = &c->v[i];
        uint32_t bytes = (v->rgb[0] << 24) | (v->rgb[1] << 16) |
                         (v->rgb[2] << 8);

        switch (c->format) {
        case GL_PRIM_COLOR3UB:
            gl_one(qts, GL_COLOR3UB_HDR, bytes);
            break;
        case GL_PRIM_COLOR4UB:
            gl_one(qts, GL_COLOR4UB_HDR, bytes | 0xff);
            break;
        default:
            gl_color3f(qts, v->rgb[0] / 255.0f, v->rgb[1] / 255.0f,
                       v->rgb[2] / 255.0f);
            break;
        }
        if (c->format == GL_PRIM_VERTEX2I) {
            gl_word(qts, GL_VERTEX2I_HDR);
            gl_word(qts, (int32_t)v->x);
            gl_word(qts, (int32_t)v->y);
        } else {
            gl_vertex2f(qts, v->x, v->y);
        }
    }
    gl_word(qts, GL_END_OF(c->begin));
}

static void test_gl_prim_matches_the_reference(const void *data)
{
    const GlPrimCase *c = data;
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_one(qts, GL_DITHER_HDR, 0);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    for (int i = 0; i < ARRAY_SIZE(c->op) && c->op[i].op != GL_PRIM_END;
         i++) {
        const GlPrimOp *op = &c->op[i];

        switch (op->op) {
        case GL_PRIM_PUSH:
            gl_word(qts, GL_PUSH_MATRIX);
            break;
        case GL_PRIM_POP:
            gl_word(qts, GL_POP_MATRIX);
            break;
        case GL_PRIM_TRANSLATE:
            gl_three(qts, GL_TRANSLATE_HDR, op->arg[0], op->arg[1],
                     op->arg[2]);
            break;
        case GL_PRIM_SCALE:
            gl_three(qts, GL_SCALE_HDR, op->arg[0], op->arg[1], op->arg[2]);
            break;
        case GL_PRIM_ROTATE:
            gl_rotate(qts, op->arg[0], op->arg[1], op->arg[2], op->arg[3]);
            break;
        default:
            gl_prim_draw(qts, c);
            break;
        }
    }
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_window_sha256(rgb, c->sha256);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Approximated: a quad, quad-strip quad or polygon is drawn as a fan of
 * triangles from its first vertex; OpenGL 1.2.1 (3.5.1) leaves the split
 * open, and the board's is not known. Smooth shading shows the split: in
 * the square below, with red, green, black and green corners, the fan
 * from the red corner gives centre (40.5, 20.5) 23.5/64 red and 20/64
 * green, 0x5e5000, where the other diagonal would give 3/64 red and 61/64
 * green; the same holds for (20.5, 40.5) by symmetry.
 */
static void test_gl_quad_splits_from_its_first_vertex(void)
{
    gl_transform_start(qts);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_SMOOTH);
    gl_word(qts, GL_BEGIN_QUADS);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex2f(qts, 0, 0);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex2f(qts, 64, 0);
    gl_color3f(qts, 0, 0, 0);
    gl_vertex2f(qts, 64, 64);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex2f(qts, 0, 64);
    gl_word(qts, GL_END_OF(GL_BEGIN_QUADS));
    g_assert_cmphex(gl_window_pixel(qts, 40, 20), ==, 0x5e5000);
    g_assert_cmphex(gl_window_pixel(qts, 20, 40), ==, 0x5e5000);
}

/*
 * A strip, fan or polygon is drawn piece by piece as its vertices arrive,
 * and the vertices the next piece needs wait in the context: a switch to
 * another context between two strip vertices loses nothing.
 */
static void test_gl_strip_spans_a_context_switch(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_word(qts, GL_BEGIN_TRI_STRIP);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex2f(qts, 8, 8);
    gl_vertex2f(qts, 8, 100);
    gl_vertex2f(qts, 60, 8);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 0.5f, 0.5f);
    gl_vertex2f(qts, 0.9f, 0.5f);
    gl_vertex2f(qts, 0.5f, 0.9f);
    gl_word(qts, GL_END_TRIANGLES);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_color3f(qts, 1, 0, 1);
    gl_vertex2f(qts, 60, 100);
    gl_word(qts, GL_END_OF(GL_BEGIN_TRI_STRIP));
    g_assert_cmphex(gl_window_pixel(qts, 20, 20), ==, 0xffff00);
    g_assert_cmphex(gl_window_pixel(qts, 50, 90), ==, 0xff00ff);
}

/*
 * The diamond-exit rule (OpenGL 1.2.1, 3.4.1) keeps a segment's starting
 * pixel and leaves out the one it ends in, so a strip's shared vertex is
 * drawn once, by the segment leaving it. A segment from centre (10.5,
 * 20.5) to centre (14.5, 20.5) covers columns 10 to 13, and one back
 * covers 11 to 14.
 */
static void test_gl_line_leaves_out_its_last_pixel(void)
{
    gl_transform_start(qts);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_LINES);
    gl_vertex2f(qts, 10.5f, 20.5f);
    gl_vertex2f(qts, 14.5f, 20.5f);
    gl_vertex2f(qts, 14.5f, 30.5f);
    gl_vertex2f(qts, 10.5f, 30.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    for (int x = 9; x <= 15; x++) {
        g_assert_cmphex(gl_window_pixel(qts, x, 20), ==,
                        x >= 10 && x <= 13 ? 0xffffff : 0);
        g_assert_cmphex(gl_window_pixel(qts, x, 30), ==,
                        x >= 11 && x <= 14 ? 0xffffff : 0);
    }
}

/*
 * Approximated: like a triangle's, a segment's fragments are bounded by
 * the viewport instead of the segment being clipped to the view volume.
 * A segment across a 32-pixel viewport's right edge stops at column 31.
 */
static void test_gl_line_bounded_by_the_viewport(void)
{
    gl_transform_start(qts);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_viewport(qts, 0, 0, 32, 32);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_LINES);
    gl_vertex2f(qts, 8.5f, 8.5f);
    gl_vertex2f(qts, 180.5f, 8.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    /* Window units under glOrtho(0, 128) in a 32-pixel viewport: / 4. */
    g_assert_cmphex(gl_window_pixel(qts, 2, 2), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 31, 2), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 32, 2), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 40, 2), ==, 0);
}

/*
 * A segment's fragments take depth and smooth colour at t, the centre's
 * projection onto the segment (3.4.1). A red-to-green segment from depth
 * -1 to 1 across a depth-tested white triangle at depth 0 shows in front
 * of it only on its first half, with the colour a quarter of the way at
 * a quarter of its length.
 */
static void test_gl_line_depth_and_colour_follow_t(void)
{
    gl_transform_start(qts);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, 0, 0, 0);
    gl_vertex3f(qts, 127, 0, 0);
    gl_vertex3f(qts, 0, 127, 0);
    gl_word(qts, GL_END_TRIANGLES);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_SMOOTH);
    gl_word(qts, GL_BEGIN_LINES);
    gl_color3f(qts, 1, 0, 0);
    /* glOrtho's near plane is at z = -1 in object units, window z 0. */
    gl_vertex3f(qts, 8.5f, 40.5f, 0.9375f);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, 72.5f, 40.5f, -0.9375f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    /* Column 24 is t 0.25: 191.25 red and 63.75 green round to bf40. */
    g_assert_cmphex(gl_window_pixel(qts, 24, 40), ==, 0xbf4000);
    /* Past the middle the segment lies behind the triangle. */
    g_assert_cmphex(gl_window_pixel(qts, 56, 40), ==, 0xffffff);
}

/*
 * Display lists. The probe's list case compiles a 16 by 16 grid of
 * triangles, one in each 8-pixel cell, alternately yellow and white, and
 * glCallList sends 0x00080c08, a pool byte offset and a size, after the
 * kernel installs the pool's page list in DMA-map slot 4 (run
 * irix-20260928T023038-8a94ce). The pool words below are laid out the way
 * that run's pool dump holds them: each list page begins with header
 * words, then the port words the list stores, packets whole. The page
 * list is the kernel's (three zero header words, then big-endian page
 * frame numbers), and the pages are neither adjacent nor in address
 * order, as the dump's were not.
 */
#define GL_LIST_EXECUTE_HDR   0x00080c08
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

static uint32_t gl_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
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
 * The probe's list as the run's pool dump holds it: four pages, each
 * beginning with its header words (previous offset, next offset and the
 * next range's size, or previous offset and 0 on the last), then whole
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
        { 0x4000, 0x1000, 0x3f9 }, { 0, 0x2000, 0x3f9 },
        { 0x1000, 0x3000, 0x121 }, { 0x2000, 0 },
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
        { 0, 0x100, 0x3f9 }, { 0x3800, 0x1400, 0x3f9 },
        { 0x100, 0x2800, 0x121 }, { 0x1400, 0 },
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
 * The probe's list-big case as run irix-20260928T111944-b1644a sent it
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
    uint32_t small[32] = { 0xf000, 0, GL_BEGIN_TRIANGLES };
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
        uint32_t header[3] = { (page - 1) << 12, (page + 1) << 12,
                               page == 13 ? 0x85 : 0x3f9 };
        unsigned int first = (page - 1) * 78;
        unsigned int n;

        if (page == 14) {
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
    uint32_t w[17] = { 0, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR,
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
    uint32_t w[] = { 0, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR, gl_bits(1),
                     gl_bits(1), gl_bits(0),
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
    uint32_t w[] = { 0, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR, gl_bits(1),
                     gl_bits(1), gl_bits(0),
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
    uint32_t last[] = { 0, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR, gl_bits(1),
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
            stl_be_p(header, 0);
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

    stl_be_p(header, 0);
    stl_be_p(header + 4, 0x1000);
    stl_be_p(header + 8, n);
    qtest_memwrite(qts, (uint64_t)GL_LIST_RANGE_FRAME(0) << 12, header,
                   sizeof(header));
    gl_call_list(qts, 0, 3);
}

static void test_gl_list_chain_over_the_word_limit(void)
{
    uint32_t w[] = { 0, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR, gl_bits(1),
                     gl_bits(1), gl_bits(0),
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
    uint32_t w[] = { 0xf000, 0, GL_BEGIN_TRIANGLES, 0x000e0100,
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
    uint32_t w[] = { 0, 0, GL_BEGIN_TRIANGLES, GL_COLOR3F_HDR, gl_bits(1),
                     gl_bits(0), gl_bits(0),
                     GL_VERTEX2F_HDR, gl_bits(-1), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(1), gl_bits(-1),
                     GL_VERTEX2F_HDR, gl_bits(0), gl_bits(1),
                     GL_END_TRIANGLES };
    g_autofree char *trace_path = NULL;
    g_autofree char *args = NULL;
    g_autofree char *trace = NULL;
    g_autofree uint32_t *rgb = NULL;
    static const uint32_t loop[] = { 0x800, 0x800, 3 };
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

/*
 * Two more queries wait on flag bit 17 and read 0x028004
 * (notes-return-mode-queries.md, "Other queries answered at 0x028004"):
 * 0xa104 with one index, which __Mgr_GetCurrentAttrState sends for
 * GL_CURRENT_COLOR (indices 4 to 7, read as floats) and the other
 * current attributes, and 0x8b08 with two words, one per histogram bin.
 * Each sets the flag once its own arguments have arrived. perfly waited
 * forever on an 0xa104 query in run irix-20260928T063220-a9945b.
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

static uint32_t gl_attribute_query(QTestState *qts, uint32_t index)
{
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    gl_word(qts, 0x0000a104);
    gl_word(qts, index);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    return qtest_readl(qts, REG_QUERY_ANSWER);
}

/*
 * __Mgr_GetCurrentAttrState reads 0xa104 indices 8 to 10 as
 * GL_CURRENT_NORMAL's floats, 11 to 14 as GL_CURRENT_TEXTURE_COORDS' and
 * 15 as GL_EDGE_FLAG, true when the answer has any of bits 0x01c00000
 * (notes-return-mode-queries.md). A new context answers OpenGL's initial
 * normal (0, 0, 1), texture coordinates (0, 0, 0, 1) and edge flag true
 * (2.7), and glNormal3f (0x070c) and glEdgeFlag (0x0f00 true, 0x1000
 * false) change the answers.
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
 * The X server draws a primitive once per box of its window's clip list,
 * loading the box into 0x147/0x148 and 0x11 into 0x14f before each pass
 * and 0 into 0x14f after the last (mgrasPolyPoint in the IRIX 6.5.30
 * impactsr.so). 0x147 packs x1 << 16 | (x2 - 1); 0x148 packs the same
 * rows counted from the bottom, (1024 - y2) << 16 | (1024 - y1 - 1). A
 * pixel outside the box is not written, so a fill that covers the box
 * paints only the box. Without this the Icon Catalog's icons painted
 * through the Background Setting panel stacked above them.
 */
static uint32_t clip_y_rows(uint32_t top, uint32_t bottom)
{
    return ((1023u - bottom) << 16) | (1023u - top);
}

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

static void test_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_context(from);

    /*
     * A query replaces the GE revision (2 on the default SI board) at
     * 0x028004 with its zero answer. The destination starts from reset, so
     * it reads 2 unless that word travels with the stream.
     */
    g_assert_cmphex(qtest_readl(to, REG_GE_REVISION), ==, 2);
    qtest_writel(from, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    qtest_writel(from, REG_CONTEXT_COMMAND, MGRAS_QUERY_TOKEN);
    qtest_writel(from, REG_CONTEXT_COMMAND, 0x3a);
    qtest_writel(from, REG_CONTEXT_COMMAND, 0);
    qtest_writel(from, REG_CONTEXT_COMMAND, 1);
    g_assert_cmphex(qtest_readl(from, REG_GE_REVISION), ==, 0);

    qtest_writel(from, REG_GE_CTRL_A, 0x200000);
    qtest_writel(from, REG_GE_DATA_A, 0x11223344);
    qtest_writel(from, REG_GE_DATA_A, 0x55667788);
    qtest_writel(from, REG_GE_CTRL_A, 0x80000002);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_2), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0);

    mgras_write_cmap_entry(from, 29, 0x66, 0x66554400);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x66));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (2u << 16) | 2u));

    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    qtest_writew(from, REG_DAC_LUT_INDEX, 0x0600);
    qtest_writeb(from, REG_DAC_LUT_DATA_2, 0xe4);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readb(to, REG_DAC_LUT_DATA_2), ==, 0xe4);
    g_assert_cmphex(qtest_readl(to, REG_GE_REVISION), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_2), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_1), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_CONTEXT_COMMAND), ==,
                    MGRAS_FINAL_TOKEN);
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (3u << 16) | 3u));
    g_assert_cmphex(mgras_test_pixel(to, 2, 2), ==, 0x66);

    g_assert_cmphex(mgras_test_display_pixel(to, 2, 2), ==, 0x00665544);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * Each PP1's XMAP register file and PP1SELECT itself travel with the
 * stream: PP1 0 in the fields the single XMAP used, PP1s 1 to 3 in the
 * version-19 section. The four PP1s differ, so a stream that carried only
 * one, or copied PP1 0 into the others, fails a read below.
 */
static void test_pp1_xmap_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-pp1-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS MGRAS_TWO_RE_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS MGRAS_TWO_RE_ARGS
                                   " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_give_pp1s_distinct_xmaps(from);
    qtest_writeb(from, REG_PP1_SELECT, PP1_SELECT(1, 0));

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readl(to, REG_PP1_SELECT), ==, PP1_SELECT(1, 0));
    for (unsigned int n = 0; n < 4; n++) {
        mgras_assert_pp1_xmap(to, mgras_pp1_select_one(n), n);
    }
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_migration_from_v9_format_default(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    /*
     * A v9 source has no per-pixel format array to send: vmstate_sgi_mgras
     * gates the "format" field at version 10, and minimum_version_id stays
     * at 9 so a v9 stream is still accepted. Confirms the destination's
     * post_load leaves a migrated-in pixel at its zero default,
     * SGI_MGRAS_PIXEL_FORMAT_CI8, matching what a v9 source always
     * painted -- not misread as RGB, which would corrupt every CI8 pixel
     * already on screen when an old-stream guest migrates onto this code.
     * Skipped unless a v9 reference binary is available; see
     * sn0-migration-test.c's QTEST_QEMU_BINARY_IOC3_V15 for the same
     * pattern.
     *
     * v9's rect fill has no FILLCOLOR_R/G/B: this exact FILLMODE value
     * (0x00100000) has bit 20 set, which v9's background check
     * (FILLMODE & 0x00500000, bits 20 and 22) reads as true, so v9 paints
     * from BKGRD_RG instead. Both v9 and v10 then resolve front[]'s low
     * byte through CMAP map 29 the same way, so the two binaries reach
     * the same display pixel by different, version-appropriate paths.
     * Byte-for-byte front[] equality across the migration, not visual
     * equivalence, is the actual claim under test: v9's raw plane and
     * v10's CI8-indexed plane are different storage conventions in
     * general, and only happen to coincide for a plain CI8 paint here.
     */
    if (!g_getenv("QTEST_QEMU_BINARY_MGRAS_V9")) {
        g_test_skip("QTEST_QEMU_BINARY_MGRAS_V9 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-mgras-v9-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_MGRAS_V9", MIGRATION_ARGS,
                          NULL, true);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_context(from);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(from, 29, 0x66, 0x66554400);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BKGRD_RG, 0x66));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_pixel(from, 5, 5), ==, 0x66);
    g_assert_cmphex(mgras_test_display_pixel(from, 5, 5), ==, 0x00665544);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(mgras_test_format(to, 5, 5), ==, MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_test_pixel(to, 5, 5), ==, 0x66);
    g_assert_cmphex(mgras_test_display_pixel(to, 5, 5), ==, 0x00665544);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_context_ack_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_PREFIX, 0);
    qtest_writel(from, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(from, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);

    sgi_migrate_stopped(from, uri);

    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(to, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(to, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                     MGRAS_PENDING_CONTEXT);
    qtest_writel(to, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_context_payload_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-context-payload-migration-XXXXXX",
                           &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_PREFIX, 0);
    qtest_writel(from, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(from, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                    MGRAS_PENDING_FLAGS);
    qtest_writel(from, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < 3; i++) {
        qtest_writel(from, REG_CONTEXT_COMMAND, i);
    }

    sgi_migrate_stopped(from, uri);

    for (int i = 3; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(to, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(to, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                    MGRAS_PENDING_CONTEXT);
    qtest_writel(to, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * glLoadMatrixf carries 16 payload words, more than the 8 the parser held
 * before, and GL state now includes the depth plane and the second colour
 * bank. A packet split by a migration resumes after its twelfth word, and
 * the depth and bank contents cross with it.
 */
static void test_gl_state_migration(void)
{
    static const float shift[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_gl_window(from, 256, 640, 128, 128, 0x11);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(from, 0, 0, 128, 128);
    gl_draw_buffer(from, 2);
    gl_clear_color(from, 1, 0, 1, 1);
    gl_word(from, GL_CLEAR_COLOR_BUFFER);
    gl_one(from, GL_CLEAR_DEPTH_HDR, 0);
    gl_word(from, GL_CLEAR_DEPTH_BUFFER);
    gl_draw_buffer(from, 1);
    gl_one(from, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_word(from, GL_LOAD_MATRIX_HDR);
    for (int i = 0; i < 12; i++) {
        gl_float(from, shift[i]);
    }

    sgi_migrate_stopped(from, uri);

    for (int i = 12; i < 16; i++) {
        gl_float(to, shift[i]);
    }
    gl_color3f(to, 1, 1, 1);
    gl_word(to, GL_BEGIN_TRIANGLES);
    gl_vertex3f(to, -1, -1, 0);
    gl_vertex3f(to, 0, -1, 0);
    gl_vertex3f(to, -1, 0, 0);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 33, 383 - 1), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 1, 383 - 1), ==, 0);

    /* Depth cleared to 0 before the migration stops LESS after it. */
    gl_one(to, GL_DEPTH_TEST_HDR, 1);
    gl_one(to, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_color3f(to, 0, 1, 0);
    gl_word(to, GL_BEGIN_TRIANGLES);
    gl_vertex3f(to, -1, -1, 0);
    gl_vertex3f(to, 0, -1, 0);
    gl_vertex3f(to, -1, 0, 0);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 33, 383 - 1), ==, 0xffffff);

    /* The back-buffer clear crossed in bank 1. */
    qtest_writeb(to, REG_XMAP_INDEX, 0);
    qtest_writel(to, REG_XMAP_BUF_SELECT, 1u);
    g_assert_cmphex(mgras_test_display_pixel(to, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * Every context's GL state crosses a migration, and so does which one is
 * current: b's clear colour applies before any switch on the destination,
 * and a's after one. So does a's half-sent glClearColor, which a finishes
 * on the destination, b's matrix stack and b's edge flag.
 */
static void test_gl_context_table_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-contexts-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_clear_color(from, 1, 0, 0, 1);
    gl_word(from, GL_CLEAR_COLOR_HDR);
    gl_float(from, 1);
    gl_float(from, 0);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_clear_color(from, 0, 1, 0, 1);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_one(from, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_three(from, GL_TRANSLATE_HDR, 8, 0, 0);
    gl_word(from, GL_PUSH_MATRIX);
    gl_three(from, GL_TRANSLATE_HDR, 0, 16, 0);
    gl_word(from, 0x00001000);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(gl_attribute_query(to, 15) & 0x01c00000, ==, 0);

    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(to, 300, 300), ==, 0x00ff00);
    gl_word(to, GL_POP_MATRIX);
    gl_mark(to, 1, 1);
    g_assert_cmphex(gl_window_pixel(to, 9, 1), ==, 0xffffff);
    gl_enter_slot(to, GL_SLOT_A);
    gl_float(to, 1);
    gl_float(to, 1);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(to, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * A primitive left open by a migration finishes on the destination: the
 * vertices each context held before it cross, in the loaded context (the
 * default slot's, the no-context entry) and in one switched out.
 */
static void test_gl_open_primitive_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-primitive-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_color3f(from, 1, 0, 0);
    gl_word(from, GL_BEGIN_TRIANGLES);
    gl_vertex2f(from, 10.25f, 10.25f);
    gl_vertex2f(from, 10.875f, 10.25f);
    gl_enter_slot(from, GL_SLOT_DEFAULT);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_color3f(from, 0, 1, 0);
    gl_word(from, GL_BEGIN_TRIANGLES);
    gl_vertex2f(from, 20.25f, 20.25f);
    gl_vertex2f(from, 20.875f, 20.25f);

    sgi_migrate_stopped(from, uri);

    gl_vertex2f(to, 20.25f, 20.875f);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(to, 20, 20), ==, 0x00ff00);
    g_assert_cmphex(gl_window_pixel(to, 10, 10), ==, 0);
    gl_enter_slot(to, GL_SLOT_A);
    gl_vertex2f(to, 10.25f, 10.875f);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(to, 10, 10), ==, 0xff0000);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_discarded_fifo_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-discard-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_context(from);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 (uint64_t)MGRAS_STATE_TOKEN(REG_PACKEDCOLOR, 252) << 32);

    sgi_migrate_stopped(from, uri);

    for (int i = 0; i < 31; i++) {
        qtest_writeq(to, REG_CONTEXT_COMMAND, 0);
    }
    /* RE_POINT reads PACKEDCOLOR only once PP1FILLMODE selects RGB. */
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00112233)));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, 0));
    g_assert_cmphex(mgras_test_pixel(to, 0, 0), ==, 0x00112233);

    qtest_quit(to);
    qtest_quit(from);
}

static void mgras_begin_partial_fifo_fill(QTestState *qts)
{
    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 0x99, 0x99663300);
    /* A rect fill reads FILLCOLOR_R, never PACKEDCOLOR, and needs the gate. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_FILLMODE, 4) << 32) |
                 MGRAS_FILLMODE_RECT_FILL_VALUE);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_FILLCOLOR_R, 4) << 32) |
                 0x99);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_BLOCK_START, 4) << 32) |
                 ((96u << 16) | 88u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_BLOCK_END, 4) << 32) |
                 ((97u << 16) | 89u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_COLORMASKLSBSA, 4) << 32) |
                 0x00ffffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_PP1FILLMODE, 4) << 32) |
                 ((uint32_t)MGRAS_LOGIC_OP_COPY << 26));

    /* One accepted execute payload word remains after this write. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_EXEC_TOKEN(REG_BLOCK_END, 8) << 32) |
                 0xdeadbeef);
}

static void mgras_finish_partial_fifo_fill(QTestState *qts)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0xcafebabe << 32) | MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                     MGRAS_HQ_FLAG_FENCE);
}

static void test_partial_fifo_fill_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *expected, *from, *to;
    uint32_t expected_surface, migrated_surface;

    tmpfs = g_dir_make_tmp("sn0-mgras-partial-fill-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    expected = qtest_init(MIGRATION_ARGS);
    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_begin_partial_fifo_fill(expected);
    mgras_begin_partial_fifo_fill(from);
    g_assert_cmphex(mgras_test_pixel(expected, 96, 88), ==, 0);
    g_assert_cmphex(mgras_test_pixel(from, 96, 88), ==, 0);
    sgi_migrate_stopped(from, uri);

    mgras_finish_partial_fifo_fill(expected);
    mgras_finish_partial_fifo_fill(to);

    /* Sample the console before a display-property query forces a refresh. */
    expected_surface = mgras_screendump_pixel(expected, 96, 88);
    migrated_surface = mgras_screendump_pixel(to, 96, 88);

    g_assert_cmphex(expected_surface, ==, 0x00996633);
    g_assert_cmphex(mgras_test_pixel(expected, 96, 88), ==, 0x99);
    g_assert_cmphex(mgras_test_wid(expected, 96, 88), ==, 0);
    g_assert_cmphex(mgras_test_pixel(expected, 98, 90), ==, 0);
    g_assert_cmphex(mgras_test_pixel(to, 96, 88), ==,
                     mgras_test_pixel(expected, 96, 88));
    g_assert_cmphex(mgras_test_wid(to, 96, 88), ==,
                     mgras_test_wid(expected, 96, 88));
    g_assert_cmphex(mgras_test_pixel(to, 98, 90), ==,
                     mgras_test_pixel(expected, 98, 90));
    g_assert_cmphex(migrated_surface, ==, expected_surface);

    qtest_quit(to);
    qtest_quit(from);
    qtest_quit(expected);
}

static void test_ge_upload_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    static const uint32_t words[4] = {
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
    };

    tmpfs = g_dir_make_tmp("sn0-mgras-ge-upload-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_GE_CTRL_A, 0x200000);
    for (int i = 0; i < ARRAY_SIZE(words) / 2; i++) {
        qtest_writel(from, REG_GE_DATA_A, words[i]);
    }

    sgi_migrate_stopped(from, uri);

    for (int i = ARRAY_SIZE(words) / 2; i < ARRAY_SIZE(words); i++) {
        qtest_writel(to, REG_GE_DATA_A, words[i]);
    }
    qtest_writel(to, REG_GE_CTRL_A, 0x80000002);
    for (int i = 0; i < ARRAY_SIZE(words) / 2; i++) {
        g_assert_cmphex(qtest_readl(to, REG_GE_READ_2), ==, words[2 * i + 1]);
        g_assert_cmphex(qtest_readl(to, REG_GE_READ_1), ==, words[2 * i]);
    }
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_2), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_1), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_visual_state_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-visual-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_context(from);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeb(from, REG_XMAP_INDEX, 3 << 2);
    qtest_writel(from, REG_XMAP_MAIN_MODE, 15 << 5);
    qtest_writeb(from, REG_XMAP_INDEX, 9 << 2);
    qtest_writel(from, REG_XMAP_MAIN_MODE, 29 << 5);

    mgras_write_cmap_position(from, 0xe00f, 16, 0x11223300);
    mgras_write_cmap_position(from, 0xe01d, 16, 0xa1b2c300);
    mgras_write_cmap_position(from, 0xe00f, 16, 0x11223300);

    mgras_tag_wid(from, 30, 30, 30, 30, 3);
    mgras_tag_wid(from, 31, 30, 31, 30, 9);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xf0));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (31u << 16) | 30u));

    g_assert_cmphex(mgras_test_display_pixel(from, 30, 30), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(from, 31, 30), ==, 0x00a1b2c3);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readb(to, REG_XMAP_INDEX), ==, 9 << 2);
    g_assert_cmphex(mgras_test_display_pixel(to, 30, 30), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(to, 31, 30), ==, 0x00a1b2c3);

    /* Continue the map-15 load at the migrated position, entry 0xf1. */
    qtest_writel(to, REG_CMAP_DATA, 0x04050600);
    mgras_tag_wid(to, 32, 30, 32, 30, 3);
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xf1));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (32u << 16) | 30u));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (32u << 16) | 30u));
    g_assert_cmphex(mgras_test_display_pixel(to, 32, 30), ==, 0x00040506);
    g_assert_cmphex(mgras_test_display_pixel(to, 30, 30), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(to, 31, 30), ==, 0x00a1b2c3);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_cursor_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-cursor-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_position(from, 0x001d, 28, 0x0000ff00);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0100));
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0100));
    qtest_writew(from, REG_VC3_RAM, 0x8000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0142));
    qtest_writew(from, REG_VC3_RAM, 0x4000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    /* The position registers name the glyph corner plus 31. */
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 50 + 31));
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 60 + 31));
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    g_assert_cmphex(mgras_test_display_pixel(from, 50, 60), ==, 0x000000ff);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(mgras_test_display_pixel(to, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(mgras_test_display_pixel(to, 51, 61), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_display_pixel(to, 52, 62), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/mgras/identity-and-aperture",
                   test_identity_and_aperture);
    qtest_add_func("sn0/origin2000/mgras/widget-reserved-halves-raz-wi",
                   test_widget_reserved_halves_raz_wi);
    qtest_add_func("sn0/origin2000/mgras/widget-control-is-plain-storage",
                   test_widget_control_is_plain_storage);
    qtest_add_func("sn0/origin2000/mgras/generic-bringup-step",
                   test_generic_bringup_step);
    qtest_add_func("sn0/origin2000/mgras/bus-status-reads-settled",
                   test_bus_status_reads_settled);
    qtest_add_func("sn0/origin2000/mgras/reset-registers-are-plain-storage",
                   test_reset_registers_are_plain_storage);
    qtest_add_func("sn0/origin2000/mgras/i2c-probe-loopback",
                   test_i2c_probe_loopback);
    qtest_add_func("sn0/origin2000/mgras/i2c-strobe-byte-read",
                   test_i2c_strobe_byte_read);
    qtest_add_func("sn0/origin2000/mgras/repp-sync-handshake",
                   test_repp_sync_handshake);
    qtest_add_func("sn0/origin2000/mgras/repp-status-byte-writes",
                   test_repp_status_byte_writes);
    qtest_add_func("sn0/origin2000/mgras/xmap-buf-select-reads-back",
                   test_xmap_buf_select_reads_back);
    qtest_add_func("sn0/origin2000/mgras/pp1-select-routes-xmap-two-re",
                   test_pp1_select_routes_xmap_on_two_re_board);
    qtest_add_func("sn0/origin2000/mgras/pp1-select-absent-pp1-one-re",
                   test_pp1_select_absent_pp1_on_one_re_board);
    qtest_add_func("sn0/origin2000/mgras/pp1-scanout-follows-first-pp1",
                   test_pp1_scanout_follows_first_pp1);
    qtest_add_func("sn0/origin2000/mgras/pp1-select-all-reads-first-pp1",
                   test_pp1_select_all_reads_first_pp1);
    qtest_add_func("sn0/origin2000/mgras/pp1-select-ignores-bits-7-to-3",
                   test_pp1_select_ignores_bits_7_to_3);
    qtest_add_func("sn0/origin2000/mgras/"
                   "pp1-select-leaves-sync-registers-shared",
                   test_pp1_select_leaves_sync_registers_shared);
    qtest_add_func("sn0/origin2000/mgras/two-re-board-draws-one-set-of-planes",
                   test_two_re_board_draws_one_set_of_planes);
    qtest_add_func("sn0/origin2000/mgras/xmap-revision-and-fifo-capacity",
                   test_xmap_revision_and_fifo_capacity);
    qtest_add_func("sn0/origin2000/mgras/xmap-buf-select-write",
                   test_xmap_buf_select_write);
    qtest_add_func("sn0/origin2000/mgras/visual-register-read-widths",
                   test_visual_register_read_widths);
    qtest_add_func("sn0/origin2000/mgras/visual-register-write-widths",
                   test_visual_register_write_widths);
    qtest_add_func("sn0/origin2000/mgras/hq-status-bringup-bit",
                   test_hq_status_bringup_bit);
    qtest_add_func("sn0/origin2000/mgras/hq-status-hiwater-poll-bit",
                   test_hq_status_hiwater_poll_bit);
    qtest_add_func("sn0/origin2000/mgras/hq-status-independent-of-error",
                   test_hq_status_independent_of_sticky_error);
    qtest_add_func("sn0/origin2000/mgras/dac-address-byte-port",
                   test_dac_address_byte_port);
    qtest_add_func("sn0/origin2000/mgras/dac-register-file-matches-ide-reset",
                   test_dac_register_file_matches_ide_reset);
    qtest_add_func("sn0/origin2000/mgras/vc3-registers-read-back",
                   test_vc3_registers_read_back);
    qtest_add_func("sn0/origin2000/mgras/dac-lut-load",
                   test_dac_lut_load);
    qtest_add_func("sn0/origin2000/mgras/dac-gamma-identity-ramp-noop",
                   test_dac_gamma_identity_ramp_leaves_colors_unchanged);
    qtest_add_func("sn0/origin2000/mgras/dac-gamma-curve-applies-to-scanout",
                   test_dac_gamma_curve_applies_to_scanout);
    qtest_add_func("sn0/origin2000/mgras/write-path-fixes-dont-raise-error",
                   test_write_path_fixes_dont_raise_the_sticky_error);
    qtest_add_func("sn0/origin2000/mgras/board-bringup-scratch-registers",
                   test_board_bringup_scratch_registers);
    qtest_add_func("sn0/origin2000/mgras/vc3-arg-byte-write",
                   test_vc3_arg_byte_write);
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
    qtest_add_func("sn0/origin2000/mgras/"
                   "rgb-pixel-words-read-back-unchanged",
                   test_rgb_pixel_words_read_back_unchanged);
    qtest_add_func("sn0/origin2000/mgras/"
                   "packedcolor-matches-fillcolor-channels",
                   test_packedcolor_matches_fillcolor_channels);
    qtest_add_func("sn0/origin2000/mgras/byte-row-pairs-rgb-survives-dma-copy",
                   test_byte_row_pairs_rgb_survives_dma_copy);
    qtest_add_func("sn0/origin2000/mgras/board-bringup-byte-poll-bit-position",
                   test_board_bringup_byte_poll_bit_position);
    qtest_add_func("sn0/origin2000/mgras/unresolved-revision-byte-read",
                   test_unresolved_revision_byte_read);
    qtest_add_func("sn0/origin2000/mgras/board-info-byte-read-no-error",
                   test_board_info_byte_read_does_not_raise_error);
    qtest_add_func("sn0/origin2000/mgras/hq-upload-readback",
                   test_hq_upload_and_readback);
    qtest_add_func("sn0/origin2000/mgras/ge-upload-readback",
                   test_ge_upload_and_readback);
    qtest_add_func("sn0/origin2000/mgras/ge-upload-slots-independent",
                   test_ge_upload_slots_do_not_cross_contaminate);
    qtest_add_func("sn0/origin2000/mgras/ge-upload-rearm-rereads",
                   test_ge_upload_rearm_rereads_from_start);
    qtest_add_func("sn0/origin2000/mgras/prefix-register-reads-back",
                   test_prefix_register_reads_back);
    qtest_add_func("sn0/origin2000/mgras/prefix-rewrite-after-entry-selection",
                   test_prefix_rewrite_after_entry_selection_does_not_poison_context);
    qtest_add_func("sn0/origin2000/mgras/context-recognizer-ordering",
                   test_context_recognizer_ordering);
    qtest_add_func("sn0/origin2000/mgras/second-switch-restarts-word-count",
                   test_second_context_switch_restarts_word_count);
    qtest_add_func("sn0/origin2000/mgras/entry-requires-prefix",
                   test_entry_requires_prefix);
    qtest_add_func("sn0/origin2000/mgras/context-command-after-image",
                   test_context_command_after_image_is_ordinary_token);
    qtest_add_func("sn0/origin2000/mgras/pending-mask-and-reassert",
                   test_pending_mask_and_reassert);
    qtest_add_func("sn0/origin2000/mgras/general-interrupt-vector",
                   test_general_interrupt_vector);
    qtest_add_func("sn0/origin2000/mgras/retrace-interrupt-vector",
                   test_retrace_interrupt_vector);
    qtest_add_func("sn0/origin2000/mgras/retrace-interrupt-needs-vc3-enable",
                   test_retrace_interrupt_needs_vc3_enable);
    qtest_add_func("sn0/origin2000/mgras/retrace-period-and-phase",
                   test_retrace_period_and_phase);
    qtest_add_func("sn0/origin2000/mgras/retrace-phase-migration",
                   test_retrace_phase_migration);
    qtest_add_func("sn0/origin2000/mgras/retrace-period-follows-vc3-tables",
                   test_retrace_period_follows_vc3_tables);
    qtest_add_func("sn0/origin2000/mgras/"
                   "retrace-programmed-period-migration",
                   test_retrace_programmed_period_migration);
    qtest_add_func("sn0/origin2000/mgras/"
                   "retrace-short-frame-runs-stock-period",
                   test_retrace_short_frame_runs_stock_period);
    qtest_add_func("sn0/origin2000/mgras/"
                   "retrace-running-period-migration",
                   test_retrace_running_period_migration);
    qtest_add_func("sn0/origin2000/mgras/"
                   "retrace-pre-dac-stream-runs-stock-period",
                   test_retrace_pre_dac_stream_runs_stock_period);
    qtest_add_func("sn0/origin2000/mgras/irq-enable-set-and-clear-ports",
                   test_irq_enable_set_and_clear_ports);
    qtest_add_func("sn0/origin2000/mgras/swap-command-raises-interrupt-10",
                   test_swap_command_raises_interrupt_10);
    qtest_add_func("sn0/origin2000/mgras/swap-command-needs-enable",
                   test_swap_command_needs_enable);
    qtest_add_func("sn0/origin2000/mgras/swap-keeps-context-switch-enable",
                   test_swap_keeps_context_switch_enable);
    qtest_add_func("sn0/origin2000/mgras/widget-error-interrupt-vector",
                   test_widget_error_interrupt_vector);
    qtest_add_func("sn0/origin2000/mgras/fifo-occupancy-always-drained",
                   test_fifo_occupancy_always_drained);
    qtest_add_func("sn0/origin2000/mgras/direct-fill-and-reset",
                   test_direct_fill_and_reset);
    qtest_add_func("sn0/origin2000/mgras/logic-op-truth-table",
                   test_logic_op_truth_table);
    qtest_add_func("sn0/origin2000/mgras/logic-op-xor-twice-restores-rect",
                   test_logic_op_xor_twice_restores_rect);
    qtest_add_func("sn0/origin2000/mgras/logic-op-xor-twice-restores-line",
                   test_logic_op_xor_twice_restores_line);
    qtest_add_func("sn0/origin2000/mgras/logic-op-xor-twice-restores-span",
                   test_logic_op_xor_twice_restores_span);
    qtest_add_func("sn0/origin2000/mgras/reset-clears-context-acks",
                   test_reset_clears_context_acknowledgements);
    qtest_add_func("sn0/origin2000/mgras/fill-mask-reads-colormasklsbsa",
                   test_fill_mask_reads_colormasklsbsa);
    qtest_add_func("sn0/origin2000/mgras/wid-from-display-id-table",
                   test_wid_from_display_id_table);
    qtest_add_func("sn0/origin2000/mgras/fill-color-reads-fillcolor-registers",
                   test_fill_color_reads_fillcolor_registers);
    qtest_add_func("sn0/origin2000/mgras/ir-alias-execute-paints-nothing",
                   test_ir_alias_execute_paints_nothing);
    qtest_add_func("sn0/origin2000/mgras/direct-glyph-row",
                   test_direct_glyph_row);
    qtest_add_func("sn0/origin2000/mgras/direct-glyph-row-char-h-only",
                   test_direct_glyph_row_char_h_only);
    qtest_add_func("sn0/origin2000/mgras/direct-glyph-row-char-l-execute",
                   test_direct_glyph_row_char_l_execute);
    qtest_add_func("sn0/origin2000/mgras/"
                   "direct-glyph-row-ci8-reads-red-channel",
                   test_direct_glyph_row_ci8_reads_red_channel);
    qtest_add_func("sn0/origin2000/mgras/direct-byte-row",
                   test_direct_byte_row);
    qtest_add_func("sn0/origin2000/mgras/"
                   "direct-byte-row-pitch-from-padding-field",
                   test_direct_byte_row_pitch_from_padding_field);
    qtest_add_func("sn0/origin2000/mgras/direct-byte-row-pairs",
                   test_direct_byte_row_pairs);
    qtest_add_func("sn0/origin2000/mgras/direct-byte-row-pairs-row-pad-bits",
                   test_direct_byte_row_pairs_row_pad_bits);
    qtest_add_func("sn0/origin2000/mgras/direct-byte-row-with-row-pad-bits",
                   test_direct_byte_row_with_row_pad_bits);
    qtest_add_func("sn0/origin2000/mgras/direct-byte-row-seed-packing",
                   test_direct_byte_row_seed_packing);
    qtest_add_func("sn0/origin2000/mgras/display-surface-conversion",
                   test_display_surface_conversion);
    qtest_add_func("sn0/origin2000/mgras/reset-refreshes-display-surface",
                   test_reset_refreshes_display_surface);
    qtest_add_func("sn0/origin2000/mgras/cursor-position-origin",
                   test_cursor_position_origin);
    qtest_add_func("sn0/origin2000/mgras/pseudocolor-scanout-looks-up-cmap",
                   test_pseudocolor_scanout_looks_up_cmap);
    qtest_add_func("sn0/origin2000/mgras/cmap-high-byte-packing",
                   test_cmap_high_byte_packing);
    qtest_add_func("sn0/origin2000/mgras/"
                   "pixel-format-tag-resolves-independently",
                   test_pixel_format_tag_resolves_independently);
    qtest_add_func("sn0/origin2000/mgras/rgb12-fill-packs-four-bit-channels",
                   test_rgb12_fill_packs_four_bit_channels);
    qtest_add_func("sn0/origin2000/mgras/"
                   "rgb12-line-and-glyph-take-top-nibbles",
                   test_rgb12_line_and_glyph_take_top_nibbles);
    qtest_add_func("sn0/origin2000/mgras/rgb12-pixel-word-reads-red-low",
                   test_rgb12_pixel_word_reads_red_low);
    qtest_add_func("sn0/origin2000/mgras/target-first-cmap-visual-selection",
                   test_target_first_cmap_visual_selection);
    qtest_add_func("sn0/origin2000/mgras/masked-out-paint-keeps-the-visual",
                   test_masked_out_paint_keeps_the_visual);
    qtest_add_func("sn0/origin2000/mgras/paint-variants-refresh-the-surface",
                   test_paint_variants_refresh_the_surface);
    qtest_add_func("sn0/origin2000/mgras/cursor-composites-when-enabled",
                   test_cursor_composites_when_enabled);
    qtest_add_func("sn0/origin2000/mgras/direct-pio-pixel-pairs",
                   test_direct_pio_pixel_pairs);
    qtest_add_func("sn0/origin2000/mgras/vc3-sram-round-trip",
                   test_vc3_sram_round_trip);
    qtest_add_func("sn0/origin2000/mgras/vc3-register-file",
                   test_vc3_register_file);
    qtest_add_func("sn0/origin2000/mgras/board-default-is-si",
                   test_board_default_is_si);
    qtest_add_func("sn0/origin2000/mgras/board-ge-revision-until-query",
                   test_board_ge_revision_until_query);
    qtest_add_func("sn0/origin2000/mgras/board-unknown-refused",
                   test_board_unknown_refused);
    for (size_t i = 0; i < ARRAY_SIZE(mgras_board_expect); i++) {
        const MgrasBoardExpect *e = &mgras_board_expect[i];
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/mgras/board-identity/%s", e->board);
        g_autofree char *screen = g_strdup_printf(
            "sn0/origin2000/mgras/board-screen-fits-row-buffers/%s",
            e->board);

        qtest_add_data_func(path, e, test_board_identity);
        qtest_add_data_func(screen, e, test_board_screen_fits_row_buffers);
        if (!e->implemented) {
            g_autofree char *refused = g_strdup_printf(
                "sn0/origin2000/mgras/board-refused/%s", e->board);

            qtest_add_data_func(refused, e, test_board_refused);
        }
    }
    qtest_add_func("sn0/origin2000/mgras/board-inventory-registers",
                   test_board_inventory_registers);
    qtest_add_func("sn0/origin2000/mgras/"
                   "cmap-selector-addresses-the-right-slot",
                   test_cmap_selector_addresses_the_right_slot);
    qtest_add_func("sn0/origin2000/mgras/cmap-selector-start-entry",
                   test_cmap_selector_start_entry);
    qtest_add_func("sn0/origin2000/mgras/int-dest-alt-offset-aliases",
                   test_int_dest_alt_offset_aliases);
    qtest_add_func("sn0/origin2000/mgras/hq-config-and-enable-registers",
                   test_hq_config_and_enable_registers);
    qtest_add_func("sn0/origin2000/mgras/fifo-lowater-interrupt",
                   test_fifo_lowater_interrupt);
    qtest_add_func("sn0/origin2000/mgras/hq-vector-registers",
                   test_hq_vector_registers);
    qtest_add_func("sn0/origin2000/mgras/fifo-threshold-registers",
                   test_fifo_threshold_registers);
    qtest_add_func("sn0/origin2000/mgras/"
                   "hq-attribute-queries-set-flag-after-arguments",
                   test_hq_attribute_queries_set_flag_after_arguments);
    qtest_add_func("sn0/origin2000/mgras/"
                   "hq-attribute-query-answers-normal-and-edge-flag",
                   test_hq_attribute_query_answers_normal_and_edge_flag);
    qtest_add_func("sn0/origin2000/mgras/hq-query-sets-flag-after-arguments",
                   test_hq_query_sets_flag_after_arguments);
    qtest_add_func("sn0/origin2000/mgras/hq-query-flag-only-from-query",
                   test_hq_query_flag_only_from_query);
    qtest_add_func("sn0/origin2000/mgras/inline-block-uses-20-bit-length",
                   test_inline_block_uses_20_bit_length);
    qtest_add_func("sn0/origin2000/mgras/gl-clear-fills-the-window",
                   test_gl_clear_fills_the_window);
    qtest_add_func("sn0/origin2000/mgras/gl-clear-ignores-the-viewport",
                   test_gl_clear_ignores_the_viewport);
    qtest_add_func("sn0/origin2000/mgras/gl-clear-needs-the-window-rectangle",
                   test_gl_clear_needs_the_window_rectangle);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-clear-shows-through-a-direct-colour-display-id",
                   test_gl_clear_shows_through_a_direct_colour_display_id);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-flat-triangle-matches-the-reference",
                   test_gl_flat_triangle_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/gl-flat-triangle-moved-vertex",
                   test_gl_flat_triangle_moved_vertex);
    qtest_add_func("sn0/origin2000/mgras/gl-flat-triangle-reversed-winding",
                   test_gl_flat_triangle_reversed_winding);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-flat-triangle-takes-the-third-vertex-colour",
                   test_gl_flat_triangle_takes_the_third_vertex_colour);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-triangle-snaps-to-the-nearest-subpixel",
                   test_gl_triangle_snaps_to_the_nearest_subpixel);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-triangle-shared-edge-centres-drawn-once",
                   test_gl_triangle_shared_edge_centres_drawn_once);
    qtest_add_func("sn0/origin2000/mgras/gl-triangle-bounded-by-the-viewport",
                   test_gl_triangle_bounded_by_the_viewport);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-triangle-with-negative-w-is-dropped",
                   test_gl_triangle_with_negative_w_is_dropped);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-ortho-matches-the-reference",
                   test_gl_ortho_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-frustum-matches-the-reference",
                   test_gl_frustum_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-load-matrix-reads-columns",
                   test_gl_load_matrix_reads_columns);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-mult-matrix-multiplies-on-the-right",
                   test_gl_mult_matrix_multiplies_on_the_right);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-matrix-mode-selects-the-matrix",
                   test_gl_matrix_mode_selects_the_matrix);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-matrix-product-is-single-precision",
                   test_gl_matrix_product_is_single_precision);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-context-setup-restores-initial-state",
                   test_gl_context_setup_restores_initial_state);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-depth-matches-the-reference",
                   test_gl_depth_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-depth-rounds-to-24-bits",
                   test_gl_depth_rounds_to_24_bits);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-depth-writes-only-when-tested-and-masked-in",
                   test_gl_depth_writes_only_when_tested_and_masked_in);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-depth-func-follows-the-enum",
                   test_gl_depth_func_follows_the_enum);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-clear-depth-sets-the-plane",
                   test_gl_clear_depth_sets_the_plane);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-triangle-clipped-at-the-far-plane",
                   test_gl_triangle_clipped_at_the_far_plane);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-smooth-matches-the-reference",
                   test_gl_smooth_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-smooth-rounds-to-the-nearest-step",
                   test_gl_smooth_rounds_to_the_nearest_step);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-smooth-is-perspective-correct",
                   test_gl_smooth_is_perspective_correct);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-back-buffer-draws-into-bank-one",
                   test_gl_back_buffer_draws_into_bank_one);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-front-and-back-draw-into-both-banks",
                   test_gl_front_and_back_draw_into_both_banks);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-front-buffer-is-the-other-bank",
                   test_gl_front_buffer_is_the_other_bank);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-bank-token-moves-the-back-buffer",
                   test_gl_bank_token_moves_the_back_buffer);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-context-image-restores-the-banks",
                   test_gl_context_image_restores_the_banks);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-swap-matches-the-reference",
                   test_gl_swap_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-swap-pending-matches-the-reference",
                   test_gl_swap_pending_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/gl-state-follows-the-context-slot",
                   test_gl_state_follows_the_context_slot);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-context-opener-resets-only-its-context",
                   test_gl_context_opener_resets_only_its_context);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-images-without-a-slot-share-one-state",
                   test_gl_images_without_a_slot_share_one_state);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-context-table-replaces-the-least-recent",
                   test_gl_context_table_replaces_the_least_recent);
    qtest_add_func("sn0/origin2000/mgras/gl-parser-follows-the-context",
                   test_gl_parser_follows_the_context);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-packet-split-by-a-context-switch",
                   test_gl_packet_split_by_a_context_switch);
    qtest_add_func("sn0/origin2000/mgras/gl-color-bytes-unpack-red-first",
                   test_gl_color_bytes_unpack_red_first);
    qtest_add_func("sn0/origin2000/mgras/gl-vertex2i-matches-the-reference",
                   test_gl_vertex2i_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-transforms-multiply-on-the-right",
                   test_gl_transforms_multiply_on_the_right);
    qtest_add_func("sn0/origin2000/mgras/gl-rotate-about-a-general-axis",
                   test_gl_rotate_about_a_general_axis);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-push-and-pop-restore-the-matrix",
                   test_gl_push_and_pop_restore_the_matrix);
    qtest_add_func("sn0/origin2000/mgras/gl-matrix-stack-holds-32",
                   test_gl_matrix_stack_holds_32);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-quad-splits-from-its-first-vertex",
                   test_gl_quad_splits_from_its_first_vertex);
    qtest_add_func("sn0/origin2000/mgras/gl-strip-spans-a-context-switch",
                   test_gl_strip_spans_a_context_switch);
    qtest_add_func("sn0/origin2000/mgras/gl-line-leaves-out-its-last-pixel",
                   test_gl_line_leaves_out_its_last_pixel);
    qtest_add_func("sn0/origin2000/mgras/gl-line-bounded-by-the-viewport",
                   test_gl_line_bounded_by_the_viewport);
    qtest_add_func("sn0/origin2000/mgras/gl-line-depth-and-colour-follow-t",
                   test_gl_line_depth_and_colour_follow_t);
    for (size_t i = 0; i < ARRAY_SIZE(gl_prim_cases); i++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/mgras/gl-reference/%s", gl_prim_cases[i].name);

        qtest_add_data_func(path, &gl_prim_cases[i],
                            test_gl_prim_matches_the_reference);
    }
    qtest_add_func("sn0/origin2000/mgras/gl-triangle-spans-a-context-switch",
                   test_gl_triangle_spans_a_context_switch);
    qtest_add_func("sn0/origin2000/mgras/gl-list-draws-the-probe-list",
                   test_gl_list_draws_the_probe_list);
    qtest_add_func("sn0/origin2000/mgras/gl-list-follows-the-chain",
                   test_gl_list_follows_the_chain);
    qtest_add_func("sn0/origin2000/mgras/gl-list-big-matches-the-reference",
                   test_gl_list_big_matches_the_reference);
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
    qtest_add_func("sn0/origin2000/mgras/hq-flags-set-and-clear",
                   test_hq_flags_set_and_clear);
    qtest_add_func("sn0/origin2000/mgras/hq-flags-fence-token-sets-bit",
                   test_hq_flags_fence_token_sets_bit);
    qtest_add_func("sn0/origin2000/mgras/hq-flags-fence-token-isolated",
                   test_hq_flags_fence_token_isolated);
    qtest_add_func("sn0/origin2000/mgras/fifo-3-byte-payload-is-one-word",
                   test_fifo_three_byte_payload_takes_one_word);
    qtest_add_func("sn0/origin2000/mgras/fifo-6-byte-payload-is-two-words",
                   test_fifo_six_byte_payload_takes_two_words);
    qtest_add_func("sn0/origin2000/mgras/sched-arg-4-round-trip",
                   test_sched_arg_4_round_trip);
    qtest_add_func("sn0/origin2000/mgras/command-accepts-hq-config-words",
                   test_command_accepts_hq_config_words);
    qtest_add_func("sn0/origin2000/mgras/context-write-after-completion",
                   test_context_write_accepted_after_completion);
    qtest_add_func("sn0/origin2000/mgras/command-fifo-state-write-headers",
                   test_command_fifo_state_write_headers);
    qtest_add_func("sn0/origin2000/mgras/"
                   "command-fifo-state-write-headers-not-yet-consumed",
                   test_command_fifo_state_write_headers_not_yet_consumed);
    qtest_add_func("sn0/origin2000/mgras/"
                   "command-fifo-skips-unaligned-and-oversized-state",
                   test_command_fifo_skips_unaligned_and_oversized_state);
    qtest_add_func("sn0/origin2000/mgras/"
                   "command-fifo-unrecognized-class-resyncs",
                   test_command_fifo_unrecognized_class_resyncs);
    qtest_add_func("sn0/origin2000/mgras/re-point-execute",
                   test_re_point_execute);
    qtest_add_func("sn0/origin2000/mgras/"
                   "re-point-execute-after-bounded-payload",
                   test_re_point_execute_after_bounded_payload);
    qtest_add_func("sn0/origin2000/mgras/re-line-execute",
                   test_re_line_execute);
    qtest_add_func("sn0/origin2000/mgras/re-line-chains-from-re-point",
                   test_re_line_chains_from_re_point);
    qtest_add_func("sn0/origin2000/mgras/re-point-ci8-reads-red-channel",
                   test_re_point_ci8_reads_red_channel);
    qtest_add_func("sn0/origin2000/mgras/re-line-ci8-reads-red-channel",
                   test_re_line_ci8_reads_red_channel);
    qtest_add_func("sn0/origin2000/mgras/"
                   "re-line-opaque-stipple-starts-at-line",
                   test_re_line_opaque_stipple_starts_at_line);
    qtest_add_func("sn0/origin2000/mgras/"
                   "re-line-transparent-stipple-keeps-clear-bits",
                   test_re_line_transparent_stipple_keeps_clear_bits);
    qtest_add_func("sn0/origin2000/mgras/"
                   "re-point-execute-applies-xywin-origin",
                   test_re_point_execute_applies_xywin_origin);
    qtest_add_func("sn0/origin2000/mgras/"
                   "re-line-execute-applies-xywin-origin",
                   test_re_line_execute_applies_xywin_origin);
    qtest_add_func("sn0/origin2000/mgras/"
                   "re-line-execute-ignores-zero-x-field-xywin",
                   test_re_line_execute_ignores_zero_x_field_xywin);
    qtest_add_func("sn0/origin2000/mgras/"
                   "re-point-execute-rejects-out-of-range-xywin-origin",
                   test_re_point_execute_rejects_out_of_range_xywin_origin);
    qtest_add_func("sn0/origin2000/mgras/"
                   "re-line-execute-rejects-out-of-range-xywin-origin",
                   test_re_line_execute_rejects_out_of_range_xywin_origin);
    qtest_add_func("sn0/origin2000/mgras/fill-honours-clip-rectangle",
                   test_fill_honours_clip_rectangle);
    qtest_add_func("sn0/origin2000/mgras/"
                   "overlay-fill-honours-clip-rectangle",
                   test_overlay_fill_honours_clip_rectangle);
    qtest_add_func("sn0/origin2000/mgras/re-line-clip-follows-xywin-origin",
                   test_re_line_clip_follows_xywin_origin);
    qtest_add_func("sn0/origin2000/mgras/re-char-h-execute",
                   test_re_char_h_execute);
    qtest_add_func("sn0/origin2000/mgras/fifo-re-blockcopy-unsupported",
                   test_fifo_re_blockcopy_unsupported);
    qtest_add_func("sn0/origin2000/mgras/migration", test_migration);
    qtest_add_func("sn0/origin2000/mgras/pp1-xmap-migration",
                   test_pp1_xmap_migration);
    qtest_add_func("sn0/origin2000/mgras/migration-from-v9-format-default",
                   test_migration_from_v9_format_default);
    qtest_add_func("sn0/origin2000/mgras/context-ack-migration",
                   test_context_ack_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-state-migration",
                   test_gl_state_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-context-table-migration",
                   test_gl_context_table_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-open-primitive-migration",
                   test_gl_open_primitive_migration);
    qtest_add_func("sn0/origin2000/mgras/context-payload-migration",
                   test_context_payload_migration);
    qtest_add_func("sn0/origin2000/mgras/discarded-fifo-migration",
                   test_discarded_fifo_migration);
    qtest_add_func("sn0/origin2000/mgras/partial-fifo-fill-migration",
                   test_partial_fifo_fill_migration);
    qtest_add_func("sn0/origin2000/mgras/ge-upload-migration",
                   test_ge_upload_migration);
    qtest_add_func("sn0/origin2000/mgras/visual-state-migration",
                   test_visual_state_migration);
    qtest_add_func("sn0/origin2000/mgras/cursor-migration",
                   test_cursor_migration);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
