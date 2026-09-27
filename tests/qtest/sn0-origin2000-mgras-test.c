/*
 * SI Viz register-contract and protocol qtests.
 *
 * Unsupported behavior remains an approximation unless the public MGRAS
 * contracts specify it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define WIDGET_BASE(w)    (SPACE_XTALK + ((uint64_t)(w) << 24))
#define SI_WIDGET         0xf
#define MGRAS_BASE           WIDGET_BASE(SI_WIDGET)
#define MGRAS_REG(offset)    (MGRAS_BASE + (offset))

/*
 * The register cases share one machine: nothing they read depends on the
 * CPU count or the memory size, so neither is spelled out here. The
 * migration cases start their own pairs and keep the fuller machine,
 * because what they check crosses a whole machine's saved state.
 */
#define MACHINE_ARGS "-M origin2000"
#define MIGRATION_ARGS "-M origin2000 -smp 8 -m 256M"

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

#define REG_FILL_CLIP_START     0x147
#define REG_FILL_CLIP_END       0x148
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

/* PP1FILLMODE bit 13; see MGRAS_RSS_FILLCOLOR_R's comment in the raster model. */
#define MGRAS_PP1FILLMODE_RGB      0x2000

/* x-test-format values; see SGI_MGRAS_PIXEL_FORMAT_* in sgi-mgras.h. */
#define MGRAS_PIXEL_FORMAT_CI8     0
#define MGRAS_PIXEL_FORMAT_RGB     1


#define MGRAS_STATE_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x18 | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))
#define MGRAS_EXEC_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x1c | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))


#define MGRAS_QOM_PATH "/machine/mgras"

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
     * the 63rd word lands (mgras-hq4.md, "Context switch and HQ flags").
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
    g_assert_cmphex(qtest_readl(qts, REG_DIRECT_COPY_READY), ==, 0x100);
}

/*
 * Inferred (see the read/write-side comments above these two cases in
 * mgras_read_internal()/mgras_write()): the reserved halves of the
 * 64-bit-aligned widget-register slots read zero and ignore writes
 * rather than raising a guest error a real access hits. Reading
 * REG_IDENTIFICATION back unchanged after writing both proves the write
 * side does not alias into the neighboring real register.
 */
static void test_widget_reserved_halves_raz_wi(void)
{
    uint32_t identification;

    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000000)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000020)), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    identification = qtest_readl(qts, REG_IDENTIFICATION);
    qtest_writel(qts, MGRAS_REG(0x000000), 0xffffffff);
    qtest_writel(qts, MGRAS_REG(0x000020), 0xffffffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000000)), ==, 0);
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

    qtest_writel(qts, REG_REPP_STATUS_1, 0x50214);
    qtest_writeb(qts, REG_XMAP_INDEX, 0x1c);
    qtest_writel(qts, REG_REPP_STATUS_3, 0x653);
    qtest_writel(qts, REG_REPP_STATUS_4, 0x10214);
    qtest_writel(qts, REG_SYNC_STATUS, 1);
    qtest_writel(qts, REG_REPP_SYNC, 0x50214);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_1), ==, 0x50214);
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

    qtest_writeb(qts, REG_REPP_STATUS_1, 0x14);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_1), ==, 0x14);

    qtest_writeb(qts, REG_REPP_STATUS_4, 0x21);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_4), ==, 0x21);

    mgras_reset();
    mgras_write_width(qts, REG_REPP_STATUS_1, 2, 0xff);
    mgras_assert_width_error(qts, REG_REPP_STATUS_1);
}

/*
 * XMAP_BUF_SELECT (docs/datasheets/mgras-hq4.md, "Swap scheduling") takes
 * only a word write, indexed by REG_XMAP_INDEX the same way as
 * REG_XMAP_MAIN_MODE. No public driver source ever reads this port back
 * (see the header comment above SGI_MGRAS_XMAP_BUF_SELECT), so this
 * proves the accepted write at two window-select indices, that other
 * widths still raise the width exception, and that a read still errors --
 * without touching swap/retrace behavior, which lives in
 * hw/misc/sgi-mgras-raster.c.
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
    mgras_read_width(qts, REG_XMAP_BUF_SELECT, 4);
    mgras_assert_width_error(qts, REG_XMAP_BUF_SELECT);
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
     * Page list: the kernel's own layout (FIX-SPEC.md section 2) -- three
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
 * reaches a copy does (FIX-SPEC.md section 1): the DDX keeps a
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
     * Each list is the kernel's own layout (FIX-SPEC.md section 2): three
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
 * Page-list base regression (FIX-SPEC.md section 2): the base pair's low
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
 * Screen-to-host read (FIX-SPEC.md section 3): the trace's own read
 * descriptor, hw-y box (0,1023)-(3,1020), pitch 4, over the seed weave
 * (FIX-SPEC.md section 0) painted directly at screen (0,0)-(3,3). Checks
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
    /* FIX-SPEC.md section 0's seed weave: one set pixel per row. */
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
     * Read descriptor: the trace's own values (FIX-SPEC.md section 3
     * table), hw-y box (0,1023)-(3,1020), pitch 4, kick 0xb9 (slot 0,
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
     * Paired write (FIX-SPEC.md section 3, step 6): reuses pitch (reg 4)
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
 * 0x071708 already returns zero at word width (see the unresolved
 * revision field comment above its read-side case); this proves the
 * newly accepted byte width gives the same answer without raising a
 * guest error.
 */
static void test_unresolved_revision_byte_read(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x071708)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, MGRAS_REG(0x071708)), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_board_info_byte_read_does_not_raise_error(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /*
     * The kernel's GFX_GETBOARDINFO handler performs a one-byte read here
     * on every OpenGL context query; the captured flow answers 0 and the
     * error state stays clear (glx-ioctl-trace.md).
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
 * Established (mgras-hq4.md): MgrasCtxsw never writes or reads a closing
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


static void test_retrace_interrupt_vector(void)
{
    const unsigned int retrace_vector = 54;
    const unsigned int error_vector = 55;
    const uint64_t expected_addr =
        ((uint64_t)HQ_WIDGET << 48) | HQ_REG_OFFSET;

    mgras_reset();

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS, retrace_vector << 16);

    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_RETRACE);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 60);

    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_RETRACE, ==,
                     MGRAS_PENDING_RETRACE);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, retrace_vector | 0x100);
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
                     MGRAS_COMMAND(REG_FILL_CLIP_START, i));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00123456));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00010203));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
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
 * than the bare 0x0080 the test above drives (blockcopy-grammar.md's
 * field table: "row padding to 8 bytes in bits 16:14, caller flags in
 * bits 7:4 and 3:0"). mgras_draw_byte_row()'s packing switch used an exact
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
 * unimplemented default and drop. FIX-SPEC.md section 0's trace values:
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
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x100));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_G, 0x200));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_B, 0x300));
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

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 50));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 60));


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
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 0x019f);
}

static void test_board_inventory_registers(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_BOARD_INVENTORY), ==, 0x21000000);
    g_assert_cmphex(qtest_readb(qts, REG_BOARD_INVENTORY), ==, 0x21);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
    qtest_writel(qts, REG_RESET_CONTROL, 0xe8000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
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

static void test_command_fifo_rejects_malformed_payload_lengths(void)
{
    mgras_reset();

    mgras_enter_context(qts);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_STATE_TOKEN(REG_PACKEDCOLOR, 6));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)MGRAS_FENCE_TOKEN << 32);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0xffffff));
    /* RGB format: RE_POINT reads PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (70u << 16) | 70u));


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_STATE_TOKEN(REG_PACKEDCOLOR, 36));
    for (int i = 0; i < 4; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     ((uint64_t)0xdeadbeef << 32) | 0xdeadbeef);
    }
    qtest_writeq(qts, REG_CONTEXT_COMMAND, (uint64_t)0xdeadbeef << 32);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (70u << 16) | 70u));
    g_assert_cmphex(mgras_test_pixel(qts, 70, 70), ==, 0x00112233);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}


/*
 * destroy2.trace shows the guest driving classes (0x8, 0xa, 0xc) framed
 * like a STATE/EXEC header with a one-word payload (see docs/analysis's
 * class-decode notes); those are now implemented as the host-memory DMA
 * image-transfer descriptor banks (mgras_raster_desc_bank_store()). This
 * test instead covers a class the model still does not implement, to
 * confirm the FIFO still consumes a still-unimplemented header's
 * declared payload and re-syncs on the next real header.
 *
 * The payload word below (0x00180004) is not an observed guest value: it
 * is chosen so that, without the fix, the pre-fix parser reads it as a
 * second bogus STATE header that swallows the next real header token as
 * its own payload, corrupting the PACKEDCOLOR write that follows and
 * making the corruption observable in a paint. Any nonzero payload_bytes
 * this class might carry has to be consumed the same way.
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00556677));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00112233));
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
 * 2,861 of the 2,862 RE_LINE executes in the retained Toolchest capture
 * chain exactly this way, tracing closed button and glyph outlines (see
 * "RE_POINT and RE_LINE addressing" in docs/datasheets/mgras-raster-
 * display.md). A vertical leg from (10,10) to (10,20) followed by a
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00998877));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00aabbcc));
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
 * XYWIN (0x115) carries a per-window screen origin that RE_POINT/RE_LINE
 * add to their own coordinate whenever its x field is nonzero: a retained
 * greeter capture's icon-panel outlines otherwise land in a "ghost
 * cluster" at the top-left instead of on their icons. OpenBSD's impact(4)
 * driver (sys/arch/sgi/dev/impactreg.h) names it IMPACTSR_CMD_XYWIN and
 * packs it through IMPACTSR_YXCOORDS(x, y) = (y << 16) | x. See
 * mgras_apply_window_origin() in hw/misc/sgi-mgras-raster.c and
 * "RE_POINT and RE_LINE addressing" in docs/datasheets/mgras-raster-
 * display.md.
 */
static void test_re_point_execute_applies_xywin_origin(void)
{
    mgras_reset();

    mgras_enter_context(qts);

    /* RGB format: RE_LINE/RE_POINT read PACKEDCOLOR only under this format. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_XYWIN, (200u << 16) | 300u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (70u << 16) | 80u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    /* XYWIN's y field (200 here) is packed top-down; the origin applies
     * it as SGI_MGRAS_HEIGHT - 1 - oy, not oy directly. */
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_XYWIN, (200u << 16) | 300u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (100u << 16) | 100u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_LINE, (105u << 16) | 105u));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    /* XYWIN's y field (200 here) is packed top-down; the origin applies
     * it as SGI_MGRAS_HEIGHT - 1 - oy, not oy directly. */
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00445566));
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
 * Nothing masks XYWIN's y field to SGI_MGRAS_HEIGHT - 1 the way the driver
 * always writes it: a guest value past 1023 drives
 * mgras_apply_window_origin()'s "(SGI_MGRAS_HEIGHT - 1) - oy" term
 * negative. Before the fix, RE_POINT's own "x >= SGI_MGRAS_WIDTH || y >=
 * SGI_MGRAS_HEIGHT" rejection let that through -- both sides of the OR are
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
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
 * loop's own "x0 < SGI_MGRAS_WIDTH && y0 < SGI_MGRAS_HEIGHT" plot check has
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00112233));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
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

    sgi_migrate_stopped(from, uri);

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
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00112233));
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
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 50));
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 60));
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
                   "dma-image-transfer-kick-byte-selects-slot",
                   test_dma_image_transfer_kick_byte_selects_slot);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-src-off-aligned-selects-page",
                   test_dma_image_transfer_src_off_aligned_selects_page);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-reads-list-entry-at-0x18",
                   test_dma_image_transfer_reads_list_entry_at_0x18);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-reads-screen-to-host",
                   test_dma_image_transfer_reads_screen_to_host);
    qtest_add_func("sn0/origin2000/mgras/"
                   "dma-image-transfer-reads-rgb-red-in-low-byte",
                   test_dma_image_transfer_reads_rgb_red_in_low_byte);
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
    qtest_add_func("sn0/origin2000/mgras/pseudocolor-scanout-looks-up-cmap",
                   test_pseudocolor_scanout_looks_up_cmap);
    qtest_add_func("sn0/origin2000/mgras/cmap-high-byte-packing",
                   test_cmap_high_byte_packing);
    qtest_add_func("sn0/origin2000/mgras/"
                   "pixel-format-tag-resolves-independently",
                   test_pixel_format_tag_resolves_independently);
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
    qtest_add_func("sn0/origin2000/mgras/hq-flags-set-and-clear",
                   test_hq_flags_set_and_clear);
    qtest_add_func("sn0/origin2000/mgras/hq-flags-fence-token-sets-bit",
                   test_hq_flags_fence_token_sets_bit);
    qtest_add_func("sn0/origin2000/mgras/hq-flags-fence-token-isolated",
                   test_hq_flags_fence_token_isolated);
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
                   "command-fifo-rejects-malformed-payload-lengths",
                   test_command_fifo_rejects_malformed_payload_lengths);
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
    qtest_add_func("sn0/origin2000/mgras/re-char-h-execute",
                   test_re_char_h_execute);
    qtest_add_func("sn0/origin2000/mgras/fifo-re-blockcopy-unsupported",
                   test_fifo_re_blockcopy_unsupported);
    qtest_add_func("sn0/origin2000/mgras/migration", test_migration);
    qtest_add_func("sn0/origin2000/mgras/migration-from-v9-format-default",
                   test_migration_from_v9_format_default);
    qtest_add_func("sn0/origin2000/mgras/context-ack-migration",
                   test_context_ack_migration);
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
