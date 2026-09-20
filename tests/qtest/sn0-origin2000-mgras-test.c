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

#define SPACE_XTALK       0x20000000000ULL
#define WIDGET_BASE(w)    (SPACE_XTALK + ((uint64_t)(w) << 24))
#define SI_WIDGET         0xf
#define SI_BASE           WIDGET_BASE(SI_WIDGET)
#define SI_REG(offset)    (SI_BASE + (offset))

#define MACHINE_ARGS "-M origin2000 -smp 8 -m 256M"


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
#define REG_CMAP_INDEX         SI_REG(0x071c88)
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


static void si_migrate(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }

        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
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

static void test_identity_and_aperture(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    uint32_t id = qtest_readl(qts, REG_IDENTIFICATION);

    g_assert_cmphex(ID_PART(id), ==, SI_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, SI_MANUFACTURER);
    g_assert_cmphex(ID_ALWAYS_ONE(id), ==, 1);


    g_assert_cmphex(qtest_readl(qts, REG_STARTUP_STATUS) & 0x1eff, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_DMA_BUSY) & 0x1f, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DIRECT_COPY_READY), ==, 0x100);

    qtest_quit(qts);
}


static void test_widget_control_is_plain_storage(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x0000300f);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x0000300f);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x00003000);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x00003000);

    qtest_quit(qts);
}


static void test_generic_bringup_step(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_BRINGUP_STEP, (520u << 10) | (65u << 2));
    g_assert_cmphex(qtest_readl(qts, REG_BRINGUP_STEP) & 0x2, ==, 0x2);

    qtest_quit(qts);
}


static void test_bus_status_reads_settled(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmpuint(((qtest_readl(qts, REG_BUS_STATUS) & 0x1f) + 1) >> 1, <, 8);

    qtest_quit(qts);
}


static void test_reset_registers_are_plain_storage(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0);
    qtest_writel(qts, REG_RESET_ARG, 0x765b900f);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0x765b900f);


    qtest_writel(qts, REG_RESET_CONTROL, 0x40000000);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0);

    qtest_quit(qts);
}


static void test_i2c_probe_loopback(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    qtest_writeb(qts, REG_I2C_SCRATCH, 0x55);
    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0x55);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeb(qts, REG_I2C_SCRATCH, 0xaa);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0xaa);

    qtest_quit(qts);
}


static void test_repp_sync_handshake(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_REPP_SYNC) & 0x00000f0f, ==,
                     0x00000101);


    qtest_writew(qts, SI_REG(0x070c30), 7);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_INDEX), ==, 7);

    qtest_writel(qts, REG_REPP_STATUS_1, 0x50214);
    qtest_writel(qts, REG_CMAP_INDEX, 0x70214);
    qtest_writel(qts, REG_REPP_STATUS_3, 0x653);
    qtest_writel(qts, REG_REPP_STATUS_4, 0x10214);
    qtest_writel(qts, REG_SYNC_STATUS, 1);
    qtest_writel(qts, REG_REPP_SYNC, 0x50214);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_1), ==, 0x50214);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_3), ==, 0x653);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_4), ==, 0x10214);
    g_assert_cmphex(qtest_readl(qts, REG_SYNC_STATUS), ==, 1);

    g_assert_cmphex(qtest_readl(qts, REG_CMAP_INDEX), ==, 7);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_SYNC) & 0x00000f0f, ==,
                     0x00000101);

    qtest_quit(qts);
}


static void test_hq_status_bringup_bit(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);

    qtest_quit(qts);
}


static void test_hq_status_hiwater_poll_bit(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x10, ==, 0x10);

    qtest_quit(qts);
}


static void test_hq_status_independent_of_sticky_error(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);


    qtest_writel(qts, SI_REG(0x090000), 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 1);


    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);

    qtest_quit(qts);
}


static void test_dac_lut_load(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_DAC_CONFIG, 0x1c);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_CONFIG), ==, 0x1c);

    qtest_writel(qts, REG_DAC_LUT_INDEX, 0x7300);
    qtest_writel(qts, REG_DAC_LUT_DATA_1, 0xff);
    qtest_writel(qts, REG_DAC_LUT_DATA_2, 0x01);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_INDEX), ==, 0x7300);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_DATA_1), ==, 0xff);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_DATA_2), ==, 0x01);

    qtest_quit(qts);
}


static void test_write_path_fixes_dont_raise_the_sticky_error(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_HQ_STATUS, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0x482b0400000000ULL);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}


static void test_board_bringup_scratch_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}


static void test_board_bringup_byte_poll_bit_position(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, SI_REG(0x071208)), ==, 0x08000000);
    g_assert_cmphex(qtest_readb(qts, SI_REG(0x071208)) & 0x8, ==, 0x8);

    qtest_quit(qts);
}

static void test_hq_upload_and_readback(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_HQ_UPLOAD, 0xff112233);
    qtest_writel(qts, REG_HQ_UPLOAD + 4, 0x00abcdef);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD), ==, 0x00112233);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD + 4), ==, 0x00abcdef);


    qtest_writel(qts, REG_HQ_UPLOAD, 0x00445566);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD), ==, 0x00445566);

    qtest_quit(qts);
}


static void test_ge_upload_and_readback(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    static const uint32_t w[6] = {
        0x11111111, 0x22222222, 0x33333333,
        0x44444444, 0x55555555, 0x66666666,
    };

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

    qtest_quit(qts);
}


static void test_ge_upload_slots_do_not_cross_contaminate(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);


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

    qtest_quit(qts);
}


static void test_ge_upload_rearm_rereads_from_start(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_GE_CTRL_A, 0x200000);
    qtest_writel(qts, REG_GE_DATA_A, 0x01020304);
    qtest_writel(qts, REG_GE_DATA_A, 0x05060708);
    qtest_writel(qts, REG_GE_CTRL_A, 0x80000002);

    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, 0x05060708);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0x01020304);

    qtest_writel(qts, REG_GE_CTRL_A, 0x80000002);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, 0x05060708);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0x01020304);

    qtest_quit(qts);
}


static void test_prefix_register_reads_back(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_PREFIX, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_PREFIX), ==, 0x12345678);

    qtest_quit(qts);
}


static void test_prefix_rewrite_after_entry_selection_does_not_poison_context(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}

static void test_context_recognizer_ordering(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_quit(qts);
}

static void test_entry_requires_prefix(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);


    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ENTRY), ==, 0);

    qtest_quit(qts);
}

static void test_context_rejects_wrong_final_token(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    for (int i = 0; i < SI_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_CONTEXT);


    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), !=, 0xf400);

    qtest_quit(qts);
}

static void test_pending_mask_and_reassert(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);


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

    qtest_quit(qts);
}


#define HUB_WIDGET              9
#define HUB_REG_OFFSET          0x90
#define HQ_WIDGET               11
#define HQ_REG_OFFSET           0xa0

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
    QTestState *qts = qtest_init(MACHINE_ARGS);
    const unsigned int retrace_vector = 54;
    const unsigned int error_vector = 55;
    const uint64_t expected_addr =
        ((uint64_t)HQ_WIDGET << 48) | HQ_REG_OFFSET;

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

    qtest_quit(qts);
}


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
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);


    for (int i = 0; i < 300; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     SI_COMMAND(REG_FILL_CLIP_START, i));
        g_assert_cmphex(qtest_readl(qts, REG_FIFO_STATUS) & 0xff, ==, 0);
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}


static void test_direct_fill_and_reset(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}

static void test_reset_clears_context_acknowledgements(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);

    qtest_system_reset(qts);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 1);

    qtest_quit(qts);
}


static void test_fill_mask_reads_colormasklsbsa(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}


static void test_ir_alias_execute(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}


static void test_direct_glyph_row(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    static const uint8_t glyph[8] = {
        0x18, 0x24, 0x42, 0x7e, 0x42, 0x42, 0x42, 0x00,
    };

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00000000));
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

    qtest_quit(qts);
}


static void test_direct_glyph_row_width_cap(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00000000));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (80u << 16) | 80u));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (103u << 16) | 80u));

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_H, 0x8000ffffu));


    g_assert_cmphex(si_test_pixel(qts, 80, 80), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 81, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 95, 80), ==, 0);

    g_assert_cmphex(si_test_pixel(qts, 96, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 103, 80), ==, 0);

    qtest_quit(qts);
}


static void test_char_l_execute_is_not_a_glyph_trigger(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (60u << 16) | 60u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (67u << 16) | 67u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_L, 0xffffffff));

    g_assert_cmphex(si_test_pixel(qts, 60, 60), ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}


static void test_direct_pio_pixel_pairs(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    static const uint32_t image[4][4] = {
        { 0x00000000, 0x00ff0000, 0x0000ff00, 0x000000ff },
        { 0x00ffffff, 0x00808080, 0x00ffff00, 0x0000ffff },
        { 0x00ff00ff, 0x00800000, 0x00008000, 0x00000080 },
        { 0x00c0c0c0, 0x00804000, 0x00408000, 0x00004080 },
    };

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

    qtest_quit(qts);
}


static void test_display_surface_conversion(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);


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

    qtest_quit(qts);
}


static void test_pseudocolor_scanout_looks_up_cmap(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x11);
    qtest_writel(qts, REG_CMAP_DATA, 0x00ecec00);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_BKGRD_RG, 0x11));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));
    g_assert_cmphex(si_test_pixel(qts, 5, 5), ==, 0x11);
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x0000ecec);


    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x7a4);
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x0000ecec);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (6u << 16) | 6u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (6u << 16) | 6u));
    g_assert_cmphex(si_test_display_pixel(qts, 6, 6), ==, 0x00332211);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x00332211);

    qtest_quit(qts);
}


static void test_masked_out_paint_keeps_index_tag(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x11);
    qtest_writel(qts, REG_CMAP_DATA, 0x00ecec00);


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
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x0000ecec);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_COLORMASKLSBSA, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_COLORMASKMSBS, 0xf0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, 0x0013f500));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (0u << 16) | 194u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (1279u << 16) | 1023u));
    g_assert_cmphex(si_test_pixel(qts, 640, 512), ==, 0x11);
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x0000ecec);


    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, SI_LOGIC_OP_COPY << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (1279u << 16) | 1023u));
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x0000ecec);


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
    g_assert_cmphex(si_test_display_pixel(qts, 640, 512), ==, 0x0000ecec);


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

    qtest_quit(qts);
}


static void test_cursor_composites_when_enabled(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x1c);
    qtest_writel(qts, REG_CMAP_DATA, 0x000000ff);


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
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0x00ff0000);
    g_assert_cmphex(si_test_display_pixel(qts, 51, 61), ==, 0x00ffffff);
    g_assert_cmphex(si_test_display_pixel(qts, 52, 62), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0000));
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0);

    qtest_quit(qts);
}

static void test_vc3_sram_round_trip(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);


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

    qtest_quit(qts);
}

static void test_vc3_register_file(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);


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

    qtest_quit(qts);
}

static void test_board_inventory_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);


    g_assert_cmphex(qtest_readl(qts, REG_BOARD_INVENTORY), ==, 0x21000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
    qtest_writel(qts, REG_RESET_CONTROL, 0xe8000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);

    qtest_quit(qts);
}

static void test_cmap_index_addresses_the_right_slot(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);


    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 5);
    qtest_writel(qts, REG_CMAP_DATA, 0x00aabbcc);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 9);
    qtest_writel(qts, REG_CMAP_DATA, 0x00112233);

    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 5);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0x00aabbcc);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 9);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0x00112233);
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_DATA), ==, 0);

    qtest_quit(qts);
}


static void test_int_dest_alt_offset_aliases(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_INT_DEST_UPPER_ALT, 0x12340000);
    qtest_writel(qts, REG_INT_DEST_LOWER_ALT, 0x5678);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_UPPER), ==, 0x12340000);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_LOWER), ==, 0x5678);

    qtest_writel(qts, REG_INT_DEST_UPPER, 0xabcd0000);
    qtest_writel(qts, REG_INT_DEST_LOWER, 0xef01);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_UPPER_ALT), ==, 0xabcd0000);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_LOWER_ALT), ==, 0xef01);

    qtest_quit(qts);
}


static void test_hq_config_and_enable_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}


static void test_fifo_lowater_interrupt(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    const unsigned int lowater_vector = 60;
    const unsigned int error_vector = 61;
    const uint64_t expected_addr =
        ((uint64_t)HQ_WIDGET << 48) | HQ_REG_OFFSET;


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

    qtest_quit(qts);
}


static void test_hq_vector_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, 0x2000f000);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, 0x9000);
    qtest_writel(qts, REG_HQ_VECTORS, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTOR_DEST_UPPER), ==,
                     0x2000f000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTOR_DEST_LOWER), ==, 0x9000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTORS), ==, 0x01020304);

    qtest_quit(qts);
}


static void test_sched_arg_4_round_trip(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_SCHED_ARG_4, 0x1885);
    g_assert_cmphex(qtest_readl(qts, REG_SCHED_ARG_4), ==, 0x1885);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}


static void test_fifo_threshold_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_FIFO_THRESHOLD_0, 0x47);
    qtest_writel(qts, REG_FIFO_THRESHOLD_1, 0x14);
    qtest_writel(qts, REG_FIFO_THRESHOLD_LAST, 0);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_0), ==, 0x47);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_1), ==, 0x14);
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_THRESHOLD_LAST), ==, 0);

    qtest_quit(qts);
}


static void test_hq_flags_set_and_clear(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0);


    qtest_writel(qts, REG_HQ_FLAGS, 0x100000);
    qtest_writel(qts, REG_HQ_FLAGS, 0x000010);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0x100010);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS_CLEAR), ==, 0x100010);


    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, 0x100000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0x000010);

    qtest_quit(qts);
}


static void test_hq_flags_fence_token_sets_bit(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==, 0);


    qtest_writel(qts, REG_CONTEXT_COMMAND, SI_FENCE_PUSH_1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_FENCE_PUSH_2);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                     SI_HQ_FLAG_FENCE);


    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, SI_HQ_FLAG_FENCE);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==, 0);

    qtest_quit(qts);
}


static void test_hq_flags_fence_token_isolated(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                     SI_HQ_FLAG_FENCE);

    qtest_quit(qts);
}


static void test_command_accepts_hq_config_words(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x0000ff04 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00008c04 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x000c0004 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x000a0204 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00080004 << 32) | 1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00000001 << 34) | 1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}


static void test_context_write_accepted_after_completion(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writel(qts, REG_CONTEXT_COMMAND, 0xf200);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x80000008);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x40000);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);

    qtest_quit(qts);
}


static void test_command_fifo_state_write_headers(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}


static void test_command_fifo_state_write_headers_not_yet_consumed(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}

static void test_command_fifo_rejects_malformed_payload_lengths(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}


static void test_re_point_execute(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}


static void test_re_line_execute(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

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

    qtest_quit(qts);
}


static void test_re_char_h_execute(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00ffffff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00000000));
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

    qtest_quit(qts);
}

/* Block-copy grammar is unknown. Retain its payload without executing it. */
static void test_re_blockcopy_stays_inert(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    /* Source: a 2x2 block at (10,10)-(11,11). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00654321));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (11u << 16) | 11u));
    g_assert_cmphex(si_test_pixel(qts, 10, 10), ==, 0x00654321);

    /* Destination: a same-size 2x2 block elsewhere, still untouched. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (50u << 16) | 50u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (51u << 16) | 51u));
    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, 0);

    /* The execute header alone, no payload yet: not dispatched. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_EXEC_TOKEN(REG_RE_BLOCKCOPY, 0x18));
    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, 0);
    /* Supply all six opaque payload words across three MMIO stores. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)((10u << 16) | 10u) << 32) | 0);
    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, 0);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 51, 51), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 52, 52), ==, 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_quit(qts);
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

    from = qtest_init(MACHINE_ARGS);
    dest_cmdline = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    si_enter_context(from);

    qtest_writel(from, REG_GE_CTRL_A, 0x200000);
    qtest_writel(from, REG_GE_DATA_A, 0x11223344);
    qtest_writel(from, REG_GE_DATA_A, 0x55667788);
    qtest_writel(from, REG_GE_CTRL_A, 0x80000002);

    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00445566));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (2u << 16) | 2u));

    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    si_migrate(from, uri);

    g_assert_cmphex(qtest_readl(to, REG_GE_READ_2), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_1), ==, 0x11223344);
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

    from = qtest_init(MACHINE_ARGS);
    dest_cmdline = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_PREFIX, 0);
    qtest_writel(from, REG_ENTRY, SI_ENTRY_WORD);
    qtest_writel(from, REG_PENDING_ACK, SI_PENDING_FLAGS);

    si_migrate(from, uri);

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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

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
    qtest_add_func("sn0/origin2000/mgras/ir-alias-execute",
                   test_ir_alias_execute);
    qtest_add_func("sn0/origin2000/mgras/direct-glyph-row",
                   test_direct_glyph_row);
    qtest_add_func("sn0/origin2000/mgras/direct-glyph-row-width-cap",
                   test_direct_glyph_row_width_cap);
    qtest_add_func("sn0/origin2000/mgras/char-l-execute-is-not-a-glyph-trigger",
                   test_char_l_execute_is_not_a_glyph_trigger);
    qtest_add_func("sn0/origin2000/mgras/display-surface-conversion",
                   test_display_surface_conversion);
    qtest_add_func("sn0/origin2000/mgras/pseudocolor-scanout-looks-up-cmap",
                   test_pseudocolor_scanout_looks_up_cmap);
    qtest_add_func("sn0/origin2000/mgras/masked-out-paint-keeps-index-tag",
                   test_masked_out_paint_keeps_index_tag);
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
    qtest_add_func("sn0/origin2000/mgras/cmap-index-addresses-the-right-slot",
                   test_cmap_index_addresses_the_right_slot);
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
    qtest_add_func("sn0/origin2000/mgras/re-point-execute",
                   test_re_point_execute);
    qtest_add_func("sn0/origin2000/mgras/re-line-execute",
                   test_re_line_execute);
    qtest_add_func("sn0/origin2000/mgras/re-char-h-execute",
                   test_re_char_h_execute);
    qtest_add_func("sn0/origin2000/mgras/re-blockcopy-stays-inert",
                   test_re_blockcopy_stays_inert);
    qtest_add_func("sn0/origin2000/mgras/migration", test_migration);
    qtest_add_func("sn0/origin2000/mgras/context-ack-migration",
                   test_context_ack_migration);

    return g_test_run();
}
