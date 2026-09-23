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
#define SI_BASE           WIDGET_BASE(SI_WIDGET)
#define SI_REG(offset)    (SI_BASE + (offset))

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


#define REG_IDENTIFICATION     SI_REG(0x000004)
#define REG_WIDGET_CONTROL     SI_REG(0x000024)
#define REG_INT_DEST_UPPER     SI_REG(0x000034)
#define REG_INT_DEST_LOWER     SI_REG(0x00003c)
#define REG_STARTUP_STATUS     SI_REG(0x008000)
#define REG_BRINGUP_STEP       SI_REG(0x011098)
#define REG_PREFIX             SI_REG(0x011000)
#define REG_RESET_ARG          SI_REG(0x011008)
#define REG_PENDING            SI_REG(0x011020)
#define REG_PENDING_ACK        SI_REG(0x011028)
#define REG_IRQ_MASK           SI_REG(0x011040)
#define REG_HQ_STATUS          SI_REG(0x020000)
#define REG_FIFO_STATUS        SI_REG(0x020008)
#define REG_BUS_STATUS         SI_REG(0x020100)
#define REG_DMA_BUSY           SI_REG(0x020200)
#define REG_DIRECT_COPY_READY  SI_REG(0x02c578)
#define REG_RESET_CONTROL      SI_REG(0x072408)
#define REG_I2C_SCRATCH        SI_REG(0x073c08)
#define REG_I2C_STROBE         SI_REG(0x073c88)
#define REG_REPP_STATUS_1      SI_REG(0x071c08)
#define REG_XMAP_INDEX         SI_REG(0x071c88)
#define REG_REPP_STATUS_3      SI_REG(0x071d00)
#define REG_REPP_STATUS_4      SI_REG(0x071d08)
#define REG_SYNC_STATUS        SI_REG(0x061038)
#define REG_REPP_SYNC          SI_REG(0x071f80)
#define REG_DAC_LUT_INDEX      SI_REG(0x071890)
#define REG_DAC_LUT_DATA_1     SI_REG(0x071810)
#define REG_DAC_LUT_DATA_2     SI_REG(0x071910)
#define REG_DAC_CONFIG         SI_REG(0x071988)
#define REG_HQ_UPLOAD           SI_REG(0x050000)
#define REG_GE_DATA_A           SI_REG(0x058048)
#define REG_GE_CTRL_A           SI_REG(0x05804c)
#define REG_GE_DATA_B           SI_REG(0x058040)
#define REG_GE_CTRL_B           SI_REG(0x058044)
#define REG_GE_READ_1           SI_REG(0x05822c)
#define REG_GE_READ_2           SI_REG(0x058230)
#define REG_ENTRY               SI_REG(0x130500)
#define REG_CONTEXT_COMMAND     SI_REG(0x120400)
#define REG_VC3_INDEXDATA       SI_REG(0x072038)
#define REG_VC3_INDEX           SI_REG(0x072008)
#define REG_VC3_DATA            SI_REG(0x0720b0)
#define REG_VC3_RAM             SI_REG(0x072190)
#define REG_BOARD_INVENTORY     SI_REG(0x072488)
#define REG_CMAP_WRITE_INDEX    SI_REG(0x070c30)
#define REG_CMAP_DATA           SI_REG(0x070d18)
#define REG_XMAP_MAIN_MODE      SI_REG(0x071e00)
#define REG_INT_DEST_UPPER_ALT  SI_REG(0x000030)
#define REG_INT_DEST_LOWER_ALT  SI_REG(0x000038)
#define REG_HQ_CONFIG_A         SI_REG(0x011030)
#define REG_HQ_CONFIG_B         SI_REG(0x011038)
#define REG_HQ_ENABLE           SI_REG(0x011050)
#define REG_HQ_ENABLE_CLEAR     SI_REG(0x011058)
#define REG_HQ_VECTOR_DEST_UPPER SI_REG(0x011060)
#define REG_HQ_VECTOR_DEST_LOWER SI_REG(0x011068)
#define REG_HQ_VECTORS           SI_REG(0x011070)
#define REG_FIFO_THRESHOLD_0     SI_REG(0x040000)
#define REG_FIFO_THRESHOLD_1     SI_REG(0x040008)
#define REG_FIFO_THRESHOLD_LAST  SI_REG(0x040030)
#define REG_HQ_FLAGS            SI_REG(0x020010)
#define REG_HQ_FLAGS_CLEAR      SI_REG(0x020018)
#define REG_SCHED_ARG_4         SI_REG(0x040070)

#define SI_ENTRY_WORD           0x000004fc
#define SI_FINAL_TOKEN           0x0000f400
#define SI_PENDING_FLAGS         0x00080000
#define SI_PENDING_CONTEXT       0x10000000
#define SI_PENDING_RETRACE       0x00000001
#define SI_PENDING_ERROR         0x00000002
#define SI_PENDING_LOWATER       0x00000004
#define SI_HQ_ENABLE_LOWATER     0x10
#define SI_CONTEXT_WORDS         63


#define SI_FENCE_PUSH_1          0x00080b04
#define SI_FENCE_PUSH_2          0x000000b1000e0400ULL
#define SI_FENCE_TOKEN           0x000e0400
#define SI_HQ_FLAG_FENCE         0x00010000

#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE(v)    ((v) & 1)

#define SI_PART             0xc003
#define SI_MANUFACTURER     0x02aa


#define SI_COMMAND(reg, value) \
    ((((uint64_t)(0x60001 | ((reg) << 6))) << 34) | (uint32_t)(value))
#define SI_COMMAND_EXEC(reg, value) \
    ((((uint64_t)(0x70001 | ((reg) << 6))) << 34) | (uint32_t)(value))


#define VC3_INDEXDATA(index, data16) \
    (((uint32_t)(index) << 24) | (((uint32_t)(data16) & 0xffff) << 8))

#define REG_FILL_CLIP_START     0x147
#define REG_FILL_CLIP_END       0x148
#define REG_FILL_COLOR          0x140
#define REG_BLOCK_START         0x46
#define REG_BLOCK_END           0x47
#define REG_PACKEDCOLOR         0x05b
#define REG_CHAR_H              0x070
#define REG_CHAR_L              0x071
#define REG_BKGRD_RG            0x140
#define REG_BKGRD_BA            0x141
#define REG_PP1FILLMODE         0x161
#define REG_COLORMASKMSBS       0x162
#define REG_RE_POINT            0x040
#define REG_RE_LINE             0x041
#define REG_RE_BLOCKCOPY        0x045

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
#define SI_FILLMODE_GLYPH_ROW    0x10400008
#define SI_FILLMODE_BYTE_XFER    0x10c00000

/*
 * Byte-transfer mode's own packing register; see
 * hw/misc/sgi-mgras-raster.c's SI_REG_BYTE_XFER_PACKING comment.
 */
#define REG_BYTE_XFER_PACKING     0x159
#define SI_BYTE_XFER_PACKING_BYTES 0x8000
#define SI_BYTE_XFER_PACKING_PAIRS 0x0080


#define SI_LOGIC_OP_COPY        0x3
#define SI_LOGIC_OP_XOR         0x6


#define SI_STATE_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x18 | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))
#define SI_EXEC_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x1c | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))


#define SI_VIZ_QOM_PATH "/machine/si-viz"

static uint64_t si_qom_get_uint(QTestState *qts, const char *property)
{
    QDict *resp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", SI_VIZ_QOM_PATH, property);
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
static void si_reset(void)
{
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static uint64_t si_read_width(QTestState *qts, uint64_t addr,
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

static void si_write_width(QTestState *qts, uint64_t addr,
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

static void si_assert_width_error(QTestState *qts, uint64_t addr)
{
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 1);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error-addr"), ==,
                    addr - SI_BASE);
}

static uint32_t si_test_pixel(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", SI_VIZ_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", SI_VIZ_QOM_PATH, y);
    return si_qom_get_uint(qts, "x-test-pixel");
}


static uint32_t si_test_display_pixel(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", SI_VIZ_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", SI_VIZ_QOM_PATH, y);
    return si_qom_get_uint(qts, "x-test-display-pixel");
}

static uint32_t si_test_wid(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", SI_VIZ_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", SI_VIZ_QOM_PATH, y);
    return si_qom_get_uint(qts, "x-test-wid");
}

static uint32_t si_screendump_pixel(QTestState *qts, uint32_t x, uint32_t y)
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


static void si_enter_context(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, REG_STARTUP_STATUS) & 0x1eff, ==, 0);

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==,
                     SI_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==, 0);

    for (int i = 0; i < SI_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_CONTEXT, ==,
                     SI_PENDING_CONTEXT);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_CONTEXT);

    qtest_writel(qts, REG_CONTEXT_COMMAND, SI_FINAL_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);
}

static void si_write_cmap_position(QTestState *qts, uint16_t selector,
                                   unsigned int position, uint32_t color)
{
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, selector);
    for (unsigned int i = 0; i < position; i++) {
        qtest_writel(qts, REG_CMAP_DATA, 0);
    }
    qtest_writel(qts, REG_CMAP_DATA, color);
}

/* Address one entry directly: bits 15:8 are the entry, bits 4:0 the map. */
static void si_write_cmap_entry(QTestState *qts, unsigned int map,
                                unsigned int entry, uint32_t color)
{
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (entry << 8) | map);
    qtest_writel(qts, REG_CMAP_DATA, color);
}

/*
 * Tag a rectangle's WID without touching its colour: COLORMASKMSBS masks
 * the WID plane and COLORMASKLSBSA the colour, so a fill with the first
 * set and the second clear writes only the tag. Leaves the colour mask
 * open for the paints that follow.
 */
static void si_tag_wid(QTestState *qts, uint32_t x0, uint32_t y0,
                       uint32_t x1, uint32_t y1, uint8_t wid)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKMSBS, 0xf0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_DRBPOINTERS, (uint32_t)wid << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (x0 << 16) | y0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (x1 << 16) | y1));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
}

static void test_identity_and_aperture(void)
{
    uint32_t id;

    si_reset();

    id = qtest_readl(qts, REG_IDENTIFICATION);
    g_assert_cmphex(ID_PART(id), ==, SI_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, SI_MANUFACTURER);
    g_assert_cmphex(ID_ALWAYS_ONE(id), ==, 1);


    g_assert_cmphex(qtest_readl(qts, REG_STARTUP_STATUS) & 0x1eff, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_DMA_BUSY) & 0x1f, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DIRECT_COPY_READY), ==, 0x100);
}


static void test_widget_control_is_plain_storage(void)
{
    si_reset();

    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x0000300f);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x0000300f);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x00003000);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x00003000);
}


static void test_generic_bringup_step(void)
{
    si_reset();

    qtest_writel(qts, REG_BRINGUP_STEP, (520u << 10) | (65u << 2));
    g_assert_cmphex(qtest_readl(qts, REG_BRINGUP_STEP) & 0x2, ==, 0x2);
}


static void test_bus_status_reads_settled(void)
{
    si_reset();

    g_assert_cmpuint(((qtest_readl(qts, REG_BUS_STATUS) & 0x1f) + 1) >> 1, <, 8);
}


static void test_reset_registers_are_plain_storage(void)
{
    si_reset();

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
    si_reset();

    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    qtest_writeb(qts, REG_I2C_SCRATCH, 0x55);
    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0x55);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeb(qts, REG_I2C_SCRATCH, 0xaa);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0xaa);
}


static void test_repp_sync_handshake(void)
{
    si_reset();

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

typedef struct SIVisualRegisterWidth {
    uint64_t addr;
    unsigned int width;
} SIVisualRegisterWidth;

static const SIVisualRegisterWidth si_visual_register_widths[] = {
    { REG_XMAP_INDEX, 1 },
    { REG_XMAP_MAIN_MODE, 4 },
    { REG_CMAP_WRITE_INDEX, 2 },
    { REG_CMAP_DATA, 4 },
};

static const unsigned int si_access_widths[] = { 1, 2, 4, 8 };

static void test_visual_register_read_widths(void)
{
    for (int reg = 0; reg < ARRAY_SIZE(si_visual_register_widths); reg++) {
        for (int width = 0; width < ARRAY_SIZE(si_access_widths); width++) {
            if (si_access_widths[width] ==
                si_visual_register_widths[reg].width) {
                continue;
            }
            si_reset();
            si_read_width(qts, si_visual_register_widths[reg].addr,
                          si_access_widths[width]);
            si_assert_width_error(qts, si_visual_register_widths[reg].addr);
        }
    }
}

static void test_visual_register_write_widths(void)
{
    for (int reg = 0; reg < ARRAY_SIZE(si_visual_register_widths); reg++) {
        for (int width = 0; width < ARRAY_SIZE(si_access_widths); width++) {
            if (si_access_widths[width] ==
                si_visual_register_widths[reg].width) {
                continue;
            }
            si_reset();
            si_write_width(qts, si_visual_register_widths[reg].addr,
                           si_access_widths[width], UINT64_MAX);
            si_assert_width_error(qts, si_visual_register_widths[reg].addr);
        }
    }
}


static void test_hq_status_bringup_bit(void)
{
    si_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);
}


static void test_hq_status_hiwater_poll_bit(void)
{
    si_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x10, ==, 0x10);
}


static void test_hq_status_independent_of_sticky_error(void)
{
    si_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);


    qtest_writel(qts, SI_REG(0x090000), 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 1);


    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);
}


static void test_dac_lut_load(void)
{
    si_reset();

    qtest_writel(qts, REG_DAC_CONFIG, 0x1c);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_CONFIG), ==, 0x1c);

    qtest_writel(qts, REG_DAC_LUT_INDEX, 0x7300);
    qtest_writel(qts, REG_DAC_LUT_DATA_1, 0xff);
    qtest_writel(qts, REG_DAC_LUT_DATA_2, 0x01);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_INDEX), ==, 0x7300);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_DATA_1), ==, 0xff);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_DATA_2), ==, 0x01);
}


static void test_write_path_fixes_dont_raise_the_sticky_error(void)
{
    si_reset();

    qtest_writel(qts, REG_HQ_STATUS, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0x482b0400000000ULL);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_board_bringup_scratch_registers(void)
{
    si_reset();

    g_assert_cmphex(qtest_readl(qts, SI_REG(0x02c574)) & 0x8, ==, 0x8);

    qtest_writel(qts, SI_REG(0x061008), 0x1088);
    qtest_writel(qts, SI_REG(0x061040), 0x210a);
    qtest_writel(qts, SI_REG(0x060008), 2);
    qtest_writel(qts, SI_REG(0x011088), 0x68f);
    qtest_writel(qts, SI_REG(0x040918), 0x200000);
    qtest_writel(qts, SI_REG(0x040928), 0);
    qtest_writeq(qts, SI_REG(0x130400), 0x482b0400000000ULL);
    qtest_writel(qts, SI_REG(0x011058), 0xffffffff);
    qtest_writel(qts, SI_REG(0x070d88), 0x3000000);
    qtest_writel(qts, SI_REG(0x071e80), 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, SI_REG(0x061008)), ==, 0x1088);
    g_assert_cmphex(qtest_readl(qts, SI_REG(0x061040)), ==, 0x210a);
    g_assert_cmphex(qtest_readl(qts, SI_REG(0x060008)), ==, 2);
    g_assert_cmphex(qtest_readl(qts, SI_REG(0x011088)), ==, 0x68f);
    g_assert_cmphex(qtest_readl(qts, SI_REG(0x040918)), ==, 0x200000);
    g_assert_cmphex(qtest_readq(qts, SI_REG(0x130400)), ==, 0x482b0400000000ULL);
    g_assert_cmphex(qtest_readl(qts, SI_REG(0x011058)), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SI_REG(0x070d88)), ==, 0x3000000);
}


static void test_board_bringup_byte_poll_bit_position(void)
{
    si_reset();

    g_assert_cmphex(qtest_readl(qts, SI_REG(0x071208)), ==, 0x08000000);
    g_assert_cmphex(qtest_readb(qts, SI_REG(0x071208)) & 0x8, ==, 0x8);
}

static void test_board_info_byte_read_does_not_raise_error(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /*
     * The kernel's GFX_GETBOARDINFO handler performs a one-byte read here
     * on every OpenGL context query; the captured flow answers 0 and the
     * error state stays clear (glx-ioctl-trace.md).
     */
    g_assert_cmphex(qtest_readb(qts, SI_REG(0x071308)), ==, 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_quit(qts);
}

static void test_hq_upload_and_readback(void)
{
    si_reset();

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

    si_reset();

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
    si_reset();

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
    si_reset();

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
    si_reset();

    qtest_writel(qts, REG_PREFIX, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_PREFIX), ==, 0x12345678);
}


static void test_prefix_rewrite_after_entry_selection_does_not_poison_context(void)
{
    si_reset();

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==,
                     SI_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);


    qtest_writel(qts, REG_PREFIX, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_PREFIX), ==, 0x12345678);

    for (int i = 0; i < SI_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_CONTEXT, ==,
                     SI_PENDING_CONTEXT);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_CONTEXT);

    qtest_writel(qts, REG_CONTEXT_COMMAND, SI_FINAL_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}

static void test_context_recognizer_ordering(void)
{
    si_reset();

    si_enter_context(qts);
}

/*
 * A second context switch must restart the context word count. Entry
 * selection resets the acknowledgement flags but must also restart the
 * word count, or the next switch's first word lands on a stale count
 * left over from the previous switch and is misread as the completion
 * token.
 */
static void test_second_context_switch_restarts_word_count(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==,
                     SI_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);

    for (int i = 0; i < SI_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }

    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_CONTEXT, ==,
                     SI_PENDING_CONTEXT);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_entry_requires_prefix(void)
{
    si_reset();

    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ENTRY), ==, 0);
}

static void test_context_rejects_wrong_final_token(void)
{
    si_reset();

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    for (int i = 0; i < SI_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_CONTEXT);


    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), !=, 0xf400);
}

static void test_pending_mask_and_reassert(void)
{
    si_reset();

    qtest_writel(qts, REG_IRQ_MASK, 0);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==,
                     SI_PENDING_FLAGS);


    qtest_writel(qts, REG_IRQ_MASK, SI_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==,
                     SI_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==, 0);
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

    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, 0);

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS, general_vector << 8);

    qtest_writel(qts, REG_IRQ_MASK, SI_PENDING_FLAGS);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);


    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, general_vector | 0x100);


    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, general_vector);

    qtest_quit(qts);
}


static void test_retrace_interrupt_vector(void)
{
    const unsigned int retrace_vector = 54;
    const unsigned int error_vector = 55;
    const uint64_t expected_addr =
        ((uint64_t)HQ_WIDGET << 48) | HQ_REG_OFFSET;

    si_reset();

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS, retrace_vector << 16);

    qtest_writel(qts, REG_IRQ_MASK, SI_PENDING_RETRACE);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 60);

    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_RETRACE, ==,
                     SI_PENDING_RETRACE);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
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


    qtest_writel(qts, REG_IRQ_MASK, SI_PENDING_ERROR);
    si_enter_context(qts);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, 0);


    qtest_writel(qts, SI_REG(0x000010), 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 1);

    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, error_vector | 0x100);

    qtest_quit(qts);
}


static void test_fifo_occupancy_always_drained(void)
{
    si_reset();

    si_enter_context(qts);


    for (int i = 0; i < 300; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     SI_COMMAND(REG_FILL_CLIP_START, i));
        g_assert_cmphex(qtest_readl(qts, REG_FIFO_STATUS) & 0xff, ==, 0);
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_direct_fill_and_reset(void)
{
    si_reset();

    si_enter_context(qts);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));

    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0x00112233);
    g_assert_cmphex(si_test_pixel(qts, 13, 13), ==, 0x00112233);

    g_assert_cmphex(si_test_pixel(qts, 20, 11), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 9, 9), ==, 0);

    /* A colour fill leaves the WID plane where it found it. */
    g_assert_cmphex(si_test_wid(qts, 11, 11), ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, SI_LOGIC_OP_XOR << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));
    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0);


    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(qtest_readl(qts, REG_PENDING), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ENTRY), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0);
}

static void test_reset_clears_context_acknowledgements(void)
{
    si_reset();

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    for (int i = 0; i < SI_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_CONTEXT);
    g_assert_true(qtest_qom_get_bool(qts, SI_VIZ_QOM_PATH,
                                     "x-test-first-ack"));
    g_assert_true(qtest_qom_get_bool(qts, SI_VIZ_QOM_PATH,
                                     "x-test-second-ack"));

    qtest_system_reset(qts);
    g_assert_false(qtest_qom_get_bool(qts, SI_VIZ_QOM_PATH,
                                      "x-test-first-ack"));
    g_assert_false(qtest_qom_get_bool(qts, SI_VIZ_QOM_PATH,
                                      "x-test-second-ack"));
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 1);
}


static void test_fill_mask_reads_colormasklsbsa(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00aa5533));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (31u << 16) | 31u));
    g_assert_cmphex(si_test_pixel(qts, 30, 30), ==, 0x00aa5533);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (41u << 16) | 41u));
    g_assert_cmphex(si_test_pixel(qts, 40, 40), ==, 0);

    /* With COLORMASKMSBS also zero the fill writes nothing at all. */
    g_assert_cmphex(si_test_wid(qts, 40, 40), ==, 0);
}


/*
 * A rectangle fill reaches the WID plane when COLORMASKMSBS is nonzero,
 * taking DRBPOINTERS bits 23:16, whether or not it also writes colour.
 * Every paint with COLORMASKMSBS clear leaves the plane alone, whatever
 * DRBPOINTERS holds at the time.
 */
static void test_wid_plane_fill_rule(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_DRBPOINTERS, 7 << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00445566));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(si_test_pixel(qts, 61, 61), ==, 0x00445566);
    g_assert_cmphex(si_test_wid(qts, 61, 61), ==, 0);

    /* The plane fill writes WID and leaves the colour it found. */
    si_tag_wid(qts, 60, 60, 63, 63, 7);
    g_assert_cmphex(si_test_wid(qts, 61, 61), ==, 7);
    g_assert_cmphex(si_test_pixel(qts, 61, 61), ==, 0x00445566);
    g_assert_cmphex(si_test_wid(qts, 64, 61), ==, 0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_DRBPOINTERS, 2 << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(si_test_pixel(qts, 61, 61), ==, 0x00112233);
    g_assert_cmphex(si_test_wid(qts, 61, 61), ==, 7);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00778899));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (63u << 16) | 63u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(si_test_pixel(qts, 61, 61), ==, 0x00778899);
    g_assert_cmphex(si_test_wid(qts, 61, 61), ==, 7);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, SI_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00010203));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_CHAR_L, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (63u << 16) | 63u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_H, 0x80000000));
    g_assert_cmphex(si_test_pixel(qts, 60, 60), ==, 0x00010203);
    g_assert_cmphex(si_test_pixel(qts, 61, 60), ==, 0x00778899);
    g_assert_cmphex(si_test_wid(qts, 60, 60), ==, 7);
    g_assert_cmphex(si_test_wid(qts, 61, 60), ==, 7);

    /* A fill with both masks open writes the colour and the tag. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_FILLMODE, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKMSBS, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_DRBPOINTERS, 4 << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(si_test_pixel(qts, 61, 61), ==, 0x00abcdef);
    g_assert_cmphex(si_test_wid(qts, 61, 61), ==, 4);
}


/*
 * A rectangle fill takes BKGRD_RG when FILLMODE has bit 20 or bit 22 set
 * and PACKEDCOLOR otherwise. The stock X server fills only through the
 * first form, with PACKEDCOLOR left at zero.
 */
static void test_fill_color_follows_fillmode(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00445566));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00aabbcc));

    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_FILLMODE, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (41u << 16) | 41u));
    g_assert_cmphex(si_test_pixel(qts, 40, 40), ==, 0x00445566);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, 0x00100000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (42u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (43u << 16) | 41u));
    g_assert_cmphex(si_test_pixel(qts, 42, 40), ==, 0x00aabbcc);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, 0x00400000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (44u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (45u << 16) | 41u));
    g_assert_cmphex(si_test_pixel(qts, 44, 40), ==, 0x00aabbcc);

    /* An unrelated FILLMODE bit leaves the fill on PACKEDCOLOR. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, 0x10000060));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (46u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (47u << 16) | 41u));
    g_assert_cmphex(si_test_pixel(qts, 46, 40), ==, 0x00445566);

    /* The colour mask still applies to whichever register was chosen. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, 0x10400008));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (48u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (49u << 16) | 41u));
    g_assert_cmphex(si_test_pixel(qts, 48, 40), ==, 0xcc);
}


static void test_ir_alias_execute(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00aabbcc));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 23u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (13u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));

    g_assert_cmphex(si_test_pixel(qts, 11, 21), ==, 0x00aabbcc);
    g_assert_cmphex(si_test_pixel(qts, 13, 20), ==, 0x00aabbcc);
    g_assert_cmphex(si_test_pixel(qts, 10, 23), ==, 0x00aabbcc);

    g_assert_cmphex(si_test_pixel(qts, 20, 21), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 10, 24), ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (30u << 16) | 31u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (31u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(si_test_pixel(qts, 30, 30), ==, 0x00aabbcc);
    g_assert_cmphex(si_test_pixel(qts, 31, 31), ==, 0x00aabbcc);
}


static void test_direct_glyph_row(void)
{
    static const uint8_t glyph[8] = {
        0x18, 0x24, 0x42, 0x7e, 0x42, 0x42, 0x42, 0x00,
    };

    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, SI_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (40u << 16) | 40u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (47u << 16) | 47u));

    for (int row = 0; row < 8; row++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     SI_COMMAND_EXEC(REG_CHAR_H, (uint32_t)glyph[row] << 24));
    }


    g_assert_cmphex(si_test_pixel(qts, 40, 40), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 43, 40), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 44, 40), ==, 0x00ffffff);

    g_assert_cmphex(si_test_pixel(qts, 40, 43), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 41, 43), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 47, 43), ==, 0);

    g_assert_cmphex(si_test_pixel(qts, 40, 47), ==, 0);

    g_assert_cmphex(si_test_pixel(qts, 50, 40), ==, 0);
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
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, SI_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (80u << 16) | 80u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (99u << 16) | 81u));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_H, 0x80019000u));

    g_assert_cmphex(si_test_pixel(qts, 80, 80), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 81, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 94, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 95, 80), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 96, 80), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 97, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 98, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 99, 80), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 100, 80), ==, 0);

    /* The single execute above must not have reached row 81 yet. */
    g_assert_cmphex(si_test_pixel(qts, 80, 81), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 85, 81), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 90, 81), ==, 0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_H, 0x04200000u));

    g_assert_cmphex(si_test_pixel(qts, 80, 81), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 85, 81), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 86, 81), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 90, 81), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 91, 81), ==, 0);
    /* Row 1's pattern differs from row 0's; no bleed from the first row. */
    g_assert_cmphex(si_test_pixel(qts, 95, 81), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 99, 81), ==, 0);
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
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, SI_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (263u << 16) | 200u));

    /*
     * Column 3: CHAR_H's upper halfword. Column 31: CHAR_H's lower
     * halfword, the last column CHAR_H sources.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_CHAR_H, 0x10000001u));
    /*
     * Column 32: CHAR_L's upper halfword, the first column CHAR_L sources.
     * Column 61: CHAR_L's lower halfword.
     */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0x80000004u));

    g_assert_cmphex(si_test_pixel(qts, 200, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 202, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 203, 200), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 204, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 230, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 231, 200), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 232, 200), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 233, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 260, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 261, 200), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 262, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 263, 200), ==, 0);

    /* The 64-column row is exactly this rect's width; nothing past it. */
    g_assert_cmphex(si_test_pixel(qts, 264, 200), ==, 0);
}


/*
 * FILLMODE_BYTE_XFER delivers eight indexed pixels per CHAR_L execute when
 * register 0x159 selects the packing (see REG_BYTE_XFER_PACKING); this is
 * also the check that 0x159 == 0x8000 keeps that eight-pixel path. An
 * 18-column-wide rect makes the row's skip into the 8-byte unit alternate
 * 0, 4, 0, ... per row (skip = (row * 4 * ceil(width / 4)) % 8), covering
 * both values in two rows. Each unit writes a fresh CHAR_H/CHAR_L pair with
 * distinct byte markers so a painted pixel's value names the byte that
 * produced it.
 */
static void test_direct_byte_row(void)
{
    si_reset();

    si_enter_context(qts);

    /* The transfer paints colour only; the WID comes from a plane fill. */
    si_tag_wid(qts, 120, 120, 137, 121, 5);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, SI_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BYTE_XFER_PACKING,
                            SI_BYTE_XFER_PACKING_BYTES));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_DRBPOINTERS, 5u << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (120u << 16) | 120u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (137u << 16) | 121u));

    /* Row 0, skip 0: three 8-byte units cover columns 0-17. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_CHAR_H, 0x00010203u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0x04050607u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_CHAR_H, 0x08090a0bu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0x0c0d0e0fu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_CHAR_H, 0x10111213u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0x14151617u));

    g_assert_cmphex(si_test_pixel(qts, 120, 120), ==, 0x00);
    g_assert_cmphex(si_test_pixel(qts, 127, 120), ==, 0x07);
    g_assert_cmphex(si_test_pixel(qts, 128, 120), ==, 0x08);
    g_assert_cmphex(si_test_pixel(qts, 137, 120), ==, 0x11);
    g_assert_cmphex(si_test_wid(qts, 120, 120), ==, 5);

    /* Row 1, skip 4: the first unit's low nibble lands on columns 0-3. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_CHAR_H, 0x28292a2bu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0x2c2d2e2fu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_CHAR_H, 0x30313233u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0x34353637u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_CHAR_H, 0x38393a3bu));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0x3c3d3e3fu));

    g_assert_cmphex(si_test_pixel(qts, 120, 121), ==, 0x2c);
    g_assert_cmphex(si_test_pixel(qts, 123, 121), ==, 0x2f);
    g_assert_cmphex(si_test_pixel(qts, 124, 121), ==, 0x30);
    g_assert_cmphex(si_test_pixel(qts, 137, 121), ==, 0x3d);
    g_assert_cmphex(si_test_wid(qts, 137, 121), ==, 5);
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

    si_enter_context(qts);

    si_tag_wid(qts, 200, 200, 205, 202, 9);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, SI_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BYTE_XFER_PACKING,
                            SI_BYTE_XFER_PACKING_PAIRS));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (205u << 16) | 202u));

    for (int row = 0; row < 3; row++) {
        for (int unit = 0; unit < 3; unit++) {
            uint8_t pixel0 = 0x10 + row * 0x10 + unit * 2;
            uint8_t pixel1 = pixel0 + 1;

            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                         SI_COMMAND(REG_CHAR_H, 0x12345600u | pixel0));
            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                         SI_COMMAND_EXEC(REG_CHAR_L, 0xabcdef00u | pixel1));
        }
    }

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 6; col++) {
            g_assert_cmphex(si_test_pixel(qts, 200 + col, 200 + row), ==,
                            0x10 + row * 0x10 + col);
        }
    }
    g_assert_cmphex(si_test_wid(qts, 200, 200), ==, 9);
    g_assert_cmphex(si_test_wid(qts, 205, 202), ==, 9);

    qtest_quit(qts);
}


static void test_direct_pio_pixel_pairs(void)
{
    static const uint32_t image[4][4] = {
        { 0x00000000, 0x00ff0000, 0x0000ff00, 0x000000ff },
        { 0x00ffffff, 0x00808080, 0x00ffff00, 0x0000ffff },
        { 0x00ff00ff, 0x00800000, 0x00008000, 0x00000080 },
        { 0x00c0c0c0, 0x00804000, 0x00408000, 0x00004080 },
    };

    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (160u << 16) | 96u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (163u << 16) | 99u));

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x += 2) {
            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                         SI_COMMAND(REG_BKGRD_RG, image[y][x]));
            qtest_writeq(qts, REG_CONTEXT_COMMAND,
                         SI_COMMAND_EXEC(REG_BKGRD_BA, image[y][x + 1]));
        }
    }

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            g_assert_cmphex(si_test_pixel(qts, 160 + x, 96 + y), ==,
                            image[y][x]);
        }
    }

    g_assert_cmphex(si_test_pixel(qts, 170, 96), ==, 0);
}


/*
 * There is no direct-colour path: a fill's low byte indexes the map its
 * WID's XMAP mode selects, and the conversion to the host surface puts
 * entry bits 31:24, 23:16 and 15:8 out as red, green and blue.
 */
static void test_display_surface_conversion(void)
{
    si_reset();

    si_enter_context(qts);

    /* WID 0 resolves through the reset mode 0x7a4, which selects map 29. */
    si_write_cmap_entry(qts, 29, 0x33, 0x33221100);
    si_write_cmap_entry(qts, 29, 0xcc, 0xccbbaa00);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));
    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0x00112233);
    g_assert_cmphex(si_test_display_pixel(qts, 11, 11), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    g_assert_cmphex(si_test_display_pixel(qts, 11, 11), ==, 0x00332211);

    g_assert_cmphex(si_test_display_pixel(qts, 20, 11), ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00aabbcc));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (31u << 16) | 31u));
    g_assert_cmphex(si_test_display_pixel(qts, 30, 30), ==, 0x00ccbbaa);

    g_assert_cmphex(si_test_display_pixel(qts, 11, 11), ==, 0x00332211);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x0));
    g_assert_cmphex(si_test_display_pixel(qts, 11, 11), ==, 0);
    g_assert_cmphex(si_test_display_pixel(qts, 30, 30), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 30, 30), ==, 0x00aabbcc);
}

static void test_reset_refreshes_display_surface(void)
{
    si_reset();

    si_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    si_write_cmap_entry(qts, 29, 0x33, 0x33221100);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (14u << 16) | 14u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (14u << 16) | 14u));
    g_assert_cmphex(si_screendump_pixel(qts, 14, 14), ==, 0x00332211);
    g_assert_cmphex(si_test_display_pixel(qts, 14, 14), ==, 0x00332211);

    qtest_system_reset(qts);
    g_assert_cmphex(si_screendump_pixel(qts, 14, 14), ==, 0);
    g_assert_cmphex(si_test_display_pixel(qts, 14, 14), ==, 0);
}


static void test_pseudocolor_scanout_looks_up_cmap(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    si_write_cmap_position(qts, 0x001d, 17, 0xecec0000);
    si_write_cmap_entry(qts, 29, 0x33, 0x44556600);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_BKGRD_RG, 0x11));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(si_test_pixel(qts, 5, 5), ==, 0x11);
    g_assert_cmphex(si_screendump_pixel(qts, 5, 5), ==, 0x00ecec00);
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x00ecec00);

    si_write_cmap_position(qts, 0x001d, 17, 0x11223300);
    g_assert_cmphex(si_screendump_pixel(qts, 5, 5), ==, 0x00112233);
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x00112233);


    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x7a4);
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x00112233);


    /* A PACKEDCOLOR fill indexes too: 0x33 is an index, not a blue byte. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (6u << 16) | 6u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (6u << 16) | 6u));
    g_assert_cmphex(si_test_display_pixel(qts, 6, 6), ==, 0x00445566);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x00445566);
}

/*
 * The guest packs CMAP entries left-justified (0xRRGGBB00); si_cmap_rgb
 * must read bits 31:8, not bits 23:0.
 */
static void test_cmap_high_byte_packing(void)
{
    si_reset();

    si_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeb(qts, REG_XMAP_INDEX, 3 << 2);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 17 << 5);

    si_write_cmap_position(qts, 0x0011, 17, 0xffffff00);

    si_tag_wid(qts, 7, 7, 7, 7, 3);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_BKGRD_RG, 0x11));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (7u << 16) | 7u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(si_test_pixel(qts, 7, 7), ==, 0x11);
    g_assert_cmphex(si_test_wid(qts, 7, 7), ==, 3);
    g_assert_cmphex(si_test_display_pixel(qts, 7, 7), ==, 0x00ffffff);
}

/*
 * Two tagged pixels, two XMAP modes, two colour maps, one shared index.
 * Each resolves through its own map, and a palette write to one map
 * leaves the other pixel alone.
 */
static void test_target_first_cmap_visual_selection(void)
{
    uint32_t native_a, native_b, wid_a, wid_b;

    si_reset();

    si_enter_context(qts);
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

    si_tag_wid(qts, 20, 20, 20, 20, 3);
    si_tag_wid(qts, 21, 20, 21, 20, 9);

    /* The paints carry a third DRBPOINTERS value and must not re-tag. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_DRBPOINTERS, 0x1f << 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0xf0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (20u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (21u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));

    native_a = si_test_pixel(qts, 20, 20);
    native_b = si_test_pixel(qts, 21, 20);
    wid_a = si_test_wid(qts, 20, 20);
    wid_b = si_test_wid(qts, 21, 20);
    g_assert_cmphex(native_a, ==, 0xf0);
    g_assert_cmphex(native_b, ==, 0xf0);
    g_assert_cmphex(wid_a, ==, 3);
    g_assert_cmphex(wid_b, ==, 9);
    g_assert_cmphex(si_test_display_pixel(qts, 20, 20), ==, 0x00112233);
    g_assert_cmphex(si_test_display_pixel(qts, 21, 20), ==, 0x00a1b2c3);

    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0xe00f);
    for (int position = 0; position < 16; position++) {
        qtest_writel(qts, REG_CMAP_DATA, 0);
    }
    qtest_writel(qts, REG_CMAP_DATA, 0x01020300);
    g_assert_cmphex(si_test_pixel(qts, 20, 20), ==, native_a);
    g_assert_cmphex(si_test_pixel(qts, 21, 20), ==, native_b);
    g_assert_cmphex(si_test_wid(qts, 20, 20), ==, wid_a);
    g_assert_cmphex(si_test_wid(qts, 21, 20), ==, wid_b);
    g_assert_cmphex(si_test_display_pixel(qts, 20, 20), ==, 0x00010203);
    g_assert_cmphex(si_test_display_pixel(qts, 21, 20), ==, 0x00a1b2c3);
}


/*
 * A paint that writes nothing -- a zero colour mask, or a logic operation
 * that reproduces the destination -- leaves the native colour, the WID and
 * therefore the displayed pixel exactly as they were.
 */
static void test_masked_out_paint_keeps_the_visual(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    si_write_cmap_position(qts, 0x001d, 17, 0xecec0000);
    si_write_cmap_entry(qts, 29, 0x33, 0x33221100);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, SI_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_PACKEDCOLOR, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_BKGRD_RG, 0x11));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (512u << 16) | 512u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (767u << 16) | 767u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(si_test_pixel(qts, 640, 512), ==, 0x11);
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_COLORMASKLSBSA, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKMSBS, 0xf0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_DRBPOINTERS, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, 0x0013f500));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (0u << 16) | 194u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (1279u << 16) | 1023u));
    g_assert_cmphex(si_test_pixel(qts, 640, 512), ==, 0x11);
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, SI_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (1279u << 16) | 1023u));
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, SI_LOGIC_OP_XOR << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKMSBS, 0x70));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (630u << 16) | 512u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (650u << 16) | 512u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)SI_EXEC_TOKEN(REG_RE_LINE, 0) << 32);
    g_assert_cmphex(si_test_pixel(qts, 640, 512), ==, 0x11);
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, SI_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0xffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (640u << 16) | 512u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (640u << 16) | 512u));
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x00332211);
}


/*
 * A window move repaints by retagging or partially recolouring pixels
 * that were already on screen, not just by filling a fresh rectangle.
 * Each of the three paints below must still leave the QMP screendump
 * agreeing with the model's own display-pixel readback.
 */
static void test_paint_variants_refresh_the_surface(void)
{
    si_reset();

    si_enter_context(qts);

    /* WID 2 resolves through map 5, WID 3 through map 6. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 2));
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 5 << 5);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 3));
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 6 << 5);
    si_write_cmap_entry(qts, 5, 0x22, 0x11223300);
    si_write_cmap_entry(qts, 6, 0x22, 0x44556600);

    /* Baseline: a colour fill tagged WID 2, index 0x22 through map 5. */
    si_tag_wid(qts, 200, 200, 207, 207, 2);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_PACKEDCOLOR, 0x22));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (207u << 16) | 207u));
    g_assert_cmphex(si_screendump_pixel(qts, 203, 203), ==,
                    si_test_display_pixel(qts, 203, 203));

    /*
     * (a) A WID-only fill retags the same rectangle to WID 3 without
     * touching its colour byte, moving it to map 6.
     */
    si_tag_wid(qts, 200, 200, 207, 207, 3);
    g_assert_cmphex(si_test_wid(qts, 203, 203), ==, 3);
    g_assert_cmphex(si_test_pixel(qts, 203, 203), ==, 0x22);
    g_assert_cmphex(si_screendump_pixel(qts, 203, 203), ==,
                    si_test_display_pixel(qts, 203, 203));

    /* (b) An IR_ALIAS fill masked to the colour's low byte only. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x99887766));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (207u << 16) | 207u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    g_assert_cmphex(si_screendump_pixel(qts, 203, 203), ==,
                    si_test_display_pixel(qts, 203, 203));

    /* (c) A FILLMODE byte-transfer row. */
    si_tag_wid(qts, 220, 220, 227, 221, 2);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, SI_FILLMODE_BYTE_XFER));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (220u << 16) | 220u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (227u << 16) | 221u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_CHAR_H, 0x22222222u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0x22222222u));
    g_assert_cmphex(si_screendump_pixel(qts, 223, 220), ==,
                    si_test_display_pixel(qts, 223, 220));
}


static void test_cursor_composites_when_enabled(void)
{
    si_reset();

    si_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    /* Entry bits 15:8 are blue, so this cursor ink is blue, not red. */
    si_write_cmap_position(qts, 0x001d, 28, 0x0000ff00);


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


    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    g_assert_cmphex(si_screendump_pixel(qts, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(si_test_display_pixel(qts, 51, 61), ==, 0x00ffffff);
    g_assert_cmphex(si_test_display_pixel(qts, 52, 62), ==, 0);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0200));
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0100));
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0x000000ff);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    g_assert_cmphex(si_screendump_pixel(qts, 50, 60), ==, 0);
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0000));
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0);
}

static void test_vc3_sram_round_trip(void)
{
    si_reset();

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
    si_reset();

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
    si_reset();

    g_assert_cmphex(qtest_readl(qts, REG_BOARD_INVENTORY), ==, 0x21000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
    qtest_writel(qts, REG_RESET_CONTROL, 0xe8000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
}

static void test_cmap_selector_addresses_the_right_slot(void)
{
    si_reset();

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
    si_reset();

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
    si_reset();

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
    si_reset();

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

    si_reset();

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS, lowater_vector);


    qtest_writel(qts, REG_IRQ_MASK, 0);


    qtest_writel(qts, REG_HQ_ENABLE, SI_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==,
                     SI_PENDING_LOWATER);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector | 0x100);


    qtest_writel(qts, REG_HQ_ENABLE_CLEAR, SI_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE) & SI_HQ_ENABLE_LOWATER,
                     ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==, 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector);


    qtest_writel(qts, REG_HQ_ENABLE, SI_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==,
                     SI_PENDING_LOWATER);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector | 0x100);


    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==, 0);
    qtest_writel(qts, REG_HQ_ENABLE, SI_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==, 0);
}


static void test_hq_vector_registers(void)
{
    si_reset();

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
    si_reset();

    qtest_writel(qts, REG_SCHED_ARG_4, 0x1885);
    g_assert_cmphex(qtest_readl(qts, REG_SCHED_ARG_4), ==, 0x1885);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_fifo_threshold_registers(void)
{
    si_reset();

    qtest_writel(qts, REG_FIFO_THRESHOLD_0, 0x47);
    qtest_writel(qts, REG_FIFO_THRESHOLD_1, 0x14);
    qtest_writel(qts, REG_FIFO_THRESHOLD_LAST, 0);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_0), ==, 0x47);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_1), ==, 0x14);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_LAST), ==, 0);
}


static void test_hq_flags_set_and_clear(void)
{
    si_reset();

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
    si_reset();

    si_enter_context(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==, 0);


    qtest_writel(qts, REG_CONTEXT_COMMAND, SI_FENCE_PUSH_1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_FENCE_PUSH_2);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                     SI_HQ_FLAG_FENCE);


    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, SI_HQ_FLAG_FENCE);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==, 0);
}


static void test_hq_flags_fence_token_isolated(void)
{
    si_reset();

    si_enter_context(qts);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                     SI_HQ_FLAG_FENCE);
}


static void test_command_accepts_hq_config_words(void)
{
    si_reset();

    si_enter_context(qts);

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
    si_reset();

    si_enter_context(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writel(qts, REG_CONTEXT_COMMAND, 0xf200);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x80000008);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x40000);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);
}


static void test_command_fifo_state_write_headers(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_PACKEDCOLOR, 4) << 32) |
                 0x00223344);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_BLOCK_START, 4) << 32) |
                 ((60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_COLORMASKLSBSA, 4) << 32) |
                 0xffffffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_PP1FILLMODE, 4) << 32) |
                 ((uint32_t)SI_LOGIC_OP_COPY << 26));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (61u << 16) | 61u));
    g_assert_cmphex(si_test_pixel(qts, 60, 60), ==, 0x00223344);
    g_assert_cmphex(si_test_pixel(qts, 61, 61), ==, 0x00223344);
    g_assert_cmphex(si_test_pixel(qts, 59, 59), ==, 0);
}


static void test_command_fifo_state_write_headers_not_yet_consumed(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_ALPHA, 4) << 32) | 0xff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_COLORMASKMSBS, 4) << 32) |
                 0x0000ffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_GLINE_XSTARTF, 4) << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_DRBPOINTERS, 4) << 32) | 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}

static void test_command_fifo_rejects_malformed_payload_lengths(void)
{
    si_reset();

    si_enter_context(qts);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_STATE_TOKEN(REG_PACKEDCOLOR, 6));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)SI_FENCE_TOKEN << 32);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                    SI_HQ_FLAG_FENCE);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, SI_HQ_FLAG_FENCE);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKLSBSA, 0xffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, SI_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (70u << 16) | 70u));


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_STATE_TOKEN(REG_PACKEDCOLOR, 36));
    for (int i = 0; i < 4; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     ((uint64_t)0xdeadbeef << 32) | 0xdeadbeef);
    }
    qtest_writeq(qts, REG_CONTEXT_COMMAND, (uint64_t)0xdeadbeef << 32);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_RE_POINT, 0));
    g_assert_cmphex(si_test_pixel(qts, 70, 70), ==, 0x00112233);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                    SI_HQ_FLAG_FENCE);
}


/*
 * destroy2.trace shows the guest driving classes (0x8, 0xa, 0xc) the model
 * does not implement, each framed like a STATE/EXEC header with a
 * one-word payload (see docs/analysis's class-decode notes). The model
 * must still consume that declared payload so the FIFO re-syncs on the
 * next real header.
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
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080404 << 32) | 0x00180004);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_PACKEDCOLOR, 4) << 32) |
                 0x00223344);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_BLOCK_START, 4) << 32) |
                 ((62u << 16) | 62u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_COLORMASKLSBSA, 4) << 32) |
                 0xffffffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_PP1FILLMODE, 4) << 32) |
                 ((uint32_t)SI_LOGIC_OP_COPY << 26));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (63u << 16) | 63u));
    g_assert_cmphex(si_test_pixel(qts, 62, 62), ==, 0x00223344);
    g_assert_cmphex(si_test_pixel(qts, 63, 63), ==, 0x00223344);
    g_assert_cmphex(si_test_pixel(qts, 61, 61), ==, 0);
}


static void test_re_point_execute(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (70u << 16) | 80u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)SI_EXEC_TOKEN(REG_RE_POINT, 0) << 32);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(si_test_pixel(qts, 70, 80), ==, 0x00abcdef);

    g_assert_cmphex(si_test_pixel(qts, 71, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 70, 81), ==, 0);
}

static void test_re_point_execute_after_bounded_payload(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00556677));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (72u << 16) | 80u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_EXEC_TOKEN(REG_RE_POINT, 8) << 32) |
                 0xdeadbeef);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0xcafebabe << 32) | 0);

    g_assert_cmphex(si_test_pixel(qts, 72, 80), ==, 0x00556677);
}


static void test_re_line_execute(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (100u << 16) | 100u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (105u << 16) | 105u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)SI_EXEC_TOKEN(REG_RE_LINE, 0) << 32);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    g_assert_cmphex(si_test_pixel(qts, 100, 100), ==, 0x00112233);
    g_assert_cmphex(si_test_pixel(qts, 102, 102), ==, 0x00112233);
    g_assert_cmphex(si_test_pixel(qts, 105, 105), ==, 0x00112233);

    g_assert_cmphex(si_test_pixel(qts, 100, 105), ==, 0);
}


static void test_re_char_h_execute(void)
{
    si_reset();

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILLMODE, SI_FILLMODE_GLYPH_ROW));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (200u << 16) | 200u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (207u << 16) | 207u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_CHAR_H, 4) << 32) |
                 ((uint32_t)0x18 << 24));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)SI_EXEC_TOKEN(REG_CHAR_H, 0) << 32);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    g_assert_cmphex(si_test_pixel(qts, 200, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 203, 200), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 204, 200), ==, 0x00ffffff);
}

static void test_fifo_re_blockcopy_unsupported(void)
{
    uint32_t native_before, wid_before, display_before, surface_before;

    si_reset();

    si_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    si_write_cmap_entry(qts, 29, 0x56, 0x12345600);

    /* Direct 0x045 remains the alias fill form. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00123456));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (50u << 16) | 50u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    native_before = si_test_pixel(qts, 50, 50);
    wid_before = si_test_wid(qts, 50, 50);
    display_before = si_test_display_pixel(qts, 50, 50);
    surface_before = si_screendump_pixel(qts, 50, 50);
    g_assert_cmphex(native_before, ==, 0x00123456);
    g_assert_cmphex(wid_before, ==, 0);
    g_assert_cmphex(display_before, ==, surface_before);

    /* A source rectangle makes the rejected copy request meaningful. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00654321));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (11u << 16) | 11u));
    g_assert_cmphex(si_test_pixel(qts, 10, 10), ==, 0x00654321);

    /* The bounded parser accepts the payload but does not execute 0x045. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_EXEC_TOKEN(REG_RE_BLOCKCOPY, 0x18));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)((10u << 16) | 10u) << 32) | 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, native_before);
    g_assert_cmphex(si_test_wid(qts, 50, 50), ==, wid_before);
    g_assert_cmphex(si_test_display_pixel(qts, 50, 50), ==, display_before);
    g_assert_cmphex(si_screendump_pixel(qts, 50, 50), ==, surface_before);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 0);

    /* Parser continuation still works after the unsupported submission. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (90u << 16) | 90u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)SI_EXEC_TOKEN(REG_RE_POINT, 0) << 32);
    g_assert_cmphex(si_test_pixel(qts, 90, 90), ==, 0x00abcdef);

    /* An incomplete FIFO submission must not survive reset. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_EXEC_TOKEN(REG_RE_BLOCKCOPY, 16));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)((10u << 16) | 10u) << 32));
    qtest_system_reset(qts);
    si_enter_context(qts);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00abcdef));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (90u << 16) | 90u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)SI_EXEC_TOKEN(REG_RE_POINT, 0) << 32);
    g_assert_cmphex(si_test_pixel(qts, 90, 90), ==, 0x00abcdef);
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

    si_enter_context(from);

    qtest_writel(from, REG_GE_CTRL_A, 0x200000);
    qtest_writel(from, REG_GE_DATA_A, 0x11223344);
    qtest_writel(from, REG_GE_DATA_A, 0x55667788);
    qtest_writel(from, REG_GE_CTRL_A, 0x80000002);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_2), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0);

    si_write_cmap_entry(from, 29, 0x66, 0x66554400);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00445566));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (2u << 16) | 2u));

    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readl(to, REG_GE_READ_2), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_1), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (3u << 16) | 3u));
    g_assert_cmphex(si_test_pixel(to, 2, 2), ==, 0x00445566);

    g_assert_cmphex(si_test_display_pixel(to, 2, 2), ==, 0x00665544);

    qtest_quit(to);
    qtest_quit(from);
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
    qtest_writel(from, REG_ENTRY, SI_ENTRY_WORD);
    qtest_writel(from, REG_PENDING_ACK, SI_PENDING_FLAGS);

    sgi_migrate_stopped(from, uri);

    for (int i = 0; i < SI_CONTEXT_WORDS; i++) {
        qtest_writel(to, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(to, REG_PENDING) & SI_PENDING_CONTEXT, ==,
                     SI_PENDING_CONTEXT);
    qtest_writel(to, REG_PENDING_ACK, SI_PENDING_CONTEXT);
    qtest_writel(to, REG_CONTEXT_COMMAND, SI_FINAL_TOKEN);
    g_assert_cmphex(qtest_readl(to, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);

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
    qtest_writel(from, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(from, REG_PENDING) & SI_PENDING_FLAGS, ==,
                    SI_PENDING_FLAGS);
    qtest_writel(from, REG_PENDING_ACK, SI_PENDING_FLAGS);
    for (int i = 0; i < 3; i++) {
        qtest_writel(from, REG_CONTEXT_COMMAND, i);
    }

    sgi_migrate_stopped(from, uri);

    for (int i = 3; i < SI_CONTEXT_WORDS; i++) {
        qtest_writel(to, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(to, REG_PENDING) & SI_PENDING_CONTEXT, ==,
                    SI_PENDING_CONTEXT);
    qtest_writel(to, REG_PENDING_ACK, SI_PENDING_CONTEXT);
    qtest_writel(to, REG_CONTEXT_COMMAND, SI_FINAL_TOKEN);
    g_assert_cmphex(qtest_readl(to, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);
    g_assert_cmphex(si_qom_get_uint(to, "x-test-error"), ==, 0);

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

    si_enter_context(from);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 (uint64_t)SI_STATE_TOKEN(REG_PACKEDCOLOR, 252) << 32);

    sgi_migrate_stopped(from, uri);

    for (int i = 0; i < 31; i++) {
        qtest_writeq(to, REG_CONTEXT_COMMAND, 0);
    }
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_RE_POINT, 0));
    g_assert_cmphex(si_test_pixel(to, 0, 0), ==, 0x00112233);

    qtest_quit(to);
    qtest_quit(from);
}

static void si_begin_partial_fifo_fill(QTestState *qts)
{
    si_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    si_write_cmap_entry(qts, 29, 0x99, 0x99663300);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_PACKEDCOLOR, 4) << 32) |
                 0x00336699);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_BLOCK_START, 4) << 32) |
                 ((96u << 16) | 88u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_BLOCK_END, 4) << 32) |
                 ((97u << 16) | 89u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_COLORMASKLSBSA, 4) << 32) |
                 0x00ffffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_STATE_TOKEN(REG_PP1FILLMODE, 4) << 32) |
                 ((uint32_t)SI_LOGIC_OP_COPY << 26));

    /* One accepted execute payload word remains after this write. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)SI_EXEC_TOKEN(REG_BLOCK_END, 8) << 32) |
                 0xdeadbeef);
}

static void si_finish_partial_fifo_fill(QTestState *qts)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0xcafebabe << 32) | SI_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                     SI_HQ_FLAG_FENCE);
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

    si_begin_partial_fifo_fill(expected);
    si_begin_partial_fifo_fill(from);
    g_assert_cmphex(si_test_pixel(expected, 96, 88), ==, 0);
    g_assert_cmphex(si_test_pixel(from, 96, 88), ==, 0);
    sgi_migrate_stopped(from, uri);

    si_finish_partial_fifo_fill(expected);
    si_finish_partial_fifo_fill(to);

    /* Sample the console before a display-property query forces a refresh. */
    expected_surface = si_screendump_pixel(expected, 96, 88);
    migrated_surface = si_screendump_pixel(to, 96, 88);

    g_assert_cmphex(expected_surface, ==, 0x00996633);
    g_assert_cmphex(si_test_pixel(expected, 96, 88), ==, 0x00336699);
    g_assert_cmphex(si_test_wid(expected, 96, 88), ==, 0);
    g_assert_cmphex(si_test_pixel(expected, 98, 90), ==, 0);
    g_assert_cmphex(si_test_pixel(to, 96, 88), ==,
                     si_test_pixel(expected, 96, 88));
    g_assert_cmphex(si_test_wid(to, 96, 88), ==,
                     si_test_wid(expected, 96, 88));
    g_assert_cmphex(si_test_pixel(to, 98, 90), ==,
                     si_test_pixel(expected, 98, 90));
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

    si_enter_context(from);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeb(from, REG_XMAP_INDEX, 3 << 2);
    qtest_writel(from, REG_XMAP_MAIN_MODE, 15 << 5);
    qtest_writeb(from, REG_XMAP_INDEX, 9 << 2);
    qtest_writel(from, REG_XMAP_MAIN_MODE, 29 << 5);

    si_write_cmap_position(from, 0xe00f, 16, 0x11223300);
    si_write_cmap_position(from, 0xe01d, 16, 0xa1b2c300);
    si_write_cmap_position(from, 0xe00f, 16, 0x11223300);

    si_tag_wid(from, 30, 30, 30, 30, 3);
    si_tag_wid(from, 31, 30, 31, 30, 9);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0xf0));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (31u << 16) | 30u));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));

    g_assert_cmphex(si_test_display_pixel(from, 30, 30), ==, 0x00112233);
    g_assert_cmphex(si_test_display_pixel(from, 31, 30), ==, 0x00a1b2c3);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readb(to, REG_XMAP_INDEX), ==, 9 << 2);
    g_assert_cmphex(si_test_display_pixel(to, 30, 30), ==, 0x00112233);
    g_assert_cmphex(si_test_display_pixel(to, 31, 30), ==, 0x00a1b2c3);

    /* Continue the map-15 load at the migrated position, entry 0xf1. */
    qtest_writel(to, REG_CMAP_DATA, 0x04050600);
    si_tag_wid(to, 32, 30, 32, 30, 3);
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0xf1));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (32u << 16) | 30u));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (32u << 16) | 30u));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(si_test_display_pixel(to, 32, 30), ==, 0x00040506);
    g_assert_cmphex(si_test_display_pixel(to, 30, 30), ==, 0x00112233);
    g_assert_cmphex(si_test_display_pixel(to, 31, 30), ==, 0x00a1b2c3);

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
    si_write_cmap_position(from, 0x001d, 28, 0x0000ff00);
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
    g_assert_cmphex(si_test_display_pixel(from, 50, 60), ==, 0x000000ff);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(si_test_display_pixel(to, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(si_test_display_pixel(to, 51, 61), ==, 0x00ffffff);
    g_assert_cmphex(si_test_display_pixel(to, 52, 62), ==, 0);

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
    qtest_add_func("sn0/origin2000/mgras/repp-sync-handshake",
                   test_repp_sync_handshake);
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
    qtest_add_func("sn0/origin2000/mgras/write-path-fixes-dont-raise-error",
                   test_write_path_fixes_dont_raise_the_sticky_error);
    qtest_add_func("sn0/origin2000/mgras/board-bringup-scratch-registers",
                   test_board_bringup_scratch_registers);
    qtest_add_func("sn0/origin2000/mgras/board-bringup-byte-poll-bit-position",
                   test_board_bringup_byte_poll_bit_position);
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
    qtest_add_func("sn0/origin2000/mgras/context-rejects-wrong-token",
                   test_context_rejects_wrong_final_token);
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
    qtest_add_func("sn0/origin2000/mgras/reset-clears-context-acks",
                   test_reset_clears_context_acknowledgements);
    qtest_add_func("sn0/origin2000/mgras/fill-mask-reads-colormasklsbsa",
                   test_fill_mask_reads_colormasklsbsa);
    qtest_add_func("sn0/origin2000/mgras/wid-plane-fill-rule",
                   test_wid_plane_fill_rule);
    qtest_add_func("sn0/origin2000/mgras/fill-color-follows-fillmode",
                   test_fill_color_follows_fillmode);
    qtest_add_func("sn0/origin2000/mgras/ir-alias-execute",
                   test_ir_alias_execute);
    qtest_add_func("sn0/origin2000/mgras/direct-glyph-row",
                   test_direct_glyph_row);
    qtest_add_func("sn0/origin2000/mgras/direct-glyph-row-char-h-only",
                   test_direct_glyph_row_char_h_only);
    qtest_add_func("sn0/origin2000/mgras/direct-glyph-row-char-l-execute",
                   test_direct_glyph_row_char_l_execute);
    qtest_add_func("sn0/origin2000/mgras/direct-byte-row",
                   test_direct_byte_row);
    qtest_add_func("sn0/origin2000/mgras/direct-byte-row-pairs",
                   test_direct_byte_row_pairs);
    qtest_add_func("sn0/origin2000/mgras/display-surface-conversion",
                   test_display_surface_conversion);
    qtest_add_func("sn0/origin2000/mgras/reset-refreshes-display-surface",
                   test_reset_refreshes_display_surface);
    qtest_add_func("sn0/origin2000/mgras/pseudocolor-scanout-looks-up-cmap",
                   test_pseudocolor_scanout_looks_up_cmap);
    qtest_add_func("sn0/origin2000/mgras/cmap-high-byte-packing",
                   test_cmap_high_byte_packing);
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
    qtest_add_func("sn0/origin2000/mgras/re-char-h-execute",
                   test_re_char_h_execute);
    qtest_add_func("sn0/origin2000/mgras/fifo-re-blockcopy-unsupported",
                   test_fifo_re_blockcopy_unsupported);
    qtest_add_func("sn0/origin2000/mgras/migration", test_migration);
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
