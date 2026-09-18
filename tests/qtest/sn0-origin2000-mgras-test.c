/*
 * SI Viz register-contract and protocol qtests, against the real Origin 2000
 * machine at Crossbow 0 widget 0xf (docs/designs/
 * si-one-mode-functional-approximation.md, "Selected machine and board").
 * These qtests hold the device to its own stated approximation, not to
 * hardware behavior the project has no evidence for; see hw/misc/sgi-mgras.c
 * and the design's own labelled approximations before extending them.
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

/* Registers sit at their slot's offset plus four, as datasheets name them. */
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

/*
 * The command-FIFO fence sequence mgrasCopyPixels/mgrasDrawPixels issue to
 * end a drawing operation: clear REG_HQ_FLAGS bit SI_HQ_FLAG_FENCE, push a
 * 32-bit token then a 64-bit token pair to REG_CONTEXT_COMMAND, then poll
 * REG_HQ_FLAGS for that bit to come back -- see the header comment on
 * SGI_MGRAS_HQ_FLAGS (include/hw/misc/sgi-mgras.h) and si_command_fifo_token
 * (hw/misc/sgi-mgras.c). The second push's low word, SI_FENCE_TOKEN, is the
 * one header this device's interpreter is evidenced to act on; the other
 * three words are unrecognized traffic this device only needs to accept
 * without raising the sticky error.
 */
#define SI_FENCE_PUSH_1          0x00080b04
#define SI_FENCE_PUSH_2          0x000000b1000e0400ULL
#define SI_FENCE_TOKEN           0x000e0400
#define SI_HQ_FLAG_FENCE         0x00010000

#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE(v)    ((v) & 1)

#define SI_PART             0xc003
#define SI_MANUFACTURER     0x02aa

/*
 * A direct 64-bit command record: payload32 | (selector << 34). Selector
 * = 0x60001 | (reg << 6) for a plain state write, or 0x70001 | (reg << 6)
 * for the execute-form write of the same register -- see si_command's own
 * header comment in hw/misc/sgi-mgras.c for the evidence and the mask-
 * constant correction against the design handoff's own worked example.
 */
#define SI_COMMAND(reg, value) \
    ((((uint64_t)(0x60001 | ((reg) << 6))) << 34) | (uint32_t)(value))
#define SI_COMMAND_EXEC(reg, value) \
    ((((uint64_t)(0x70001 | ((reg) << 6))) << 34) | (uint32_t)(value))

/* The VC3's indexed write port: register index in the top byte, 16-bit
 * value in bits 23:8. */
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
/* Same physical register as REG_RE_BLOCKCOPY above under its other public
 * name; see the header comment on SI_REG_IR_ALIAS, hw/misc/sgi-mgras.c. */
#define REG_IR_ALIAS            0x045
#define REG_ALPHA               0x05f
#define REG_COLORMASKLSBSA      0x163
#define REG_GLINE_XSTARTF       0x00c
#define REG_DRBPOINTERS         0x16d

/* X11-style GX logic-op values, PP1FILLMODE's own top nibble. */
#define SI_LOGIC_OP_COPY        0x3
#define SI_LOGIC_OP_XOR         0x6

/*
 * Command-FIFO token-header builders: class<<16 | reg<<8 | payload_bytes,
 * with reg's own bit 8 folded into the class byte's bit 0 -- see the
 * header comment on SI_CMD_HDR_CLASS_STATE, hw/misc/sgi-mgras.c. A state
 * header (class 0x18/0x19) triggers a plain register write once its
 * declared payload has arrived; an execute header (class 0x1c/0x1d)
 * triggers the matching drawing primitive, the same register roles the
 * direct-record macros above reach through their own selector encoding.
 */
#define SI_STATE_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x18 | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))
#define SI_EXEC_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x1c | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))

/*
 * sgi_machine_build_mgras (hw/mips/sgi/sn-machine.c) parents SI Viz
 * directly under the machine, the same way an io_boards[] entry is
 * ("io-board<N>"): the board-devices rewrite that replaced the old
 * standalone crosstalk assembly -- which owned a "/machine/xtalk"
 * container of its own -- has no such container any more.
 */
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

/*
 * x-test-display-pixel reuses x-test-pixel's own x/y coordinate
 * properties (hw/misc/sgi-mgras.c's si_test_get_display_pixel) and, on
 * read, forces a synchronous gfx_update before reporting display_buffer
 * -- the QEMU display surface front[] converts into, not front[] itself.
 */
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

/*
 * Migrate and block until completed or failed, mirroring
 * tests/qtest/sn0-migration-test.c's migrate_stopped_vm.
 */
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

    /* Idle startup status reads clear under its selected mask. */
    g_assert_cmphex(qtest_readl(qts, REG_STARTUP_STATUS) & 0x1eff, ==, 0);
    /* No-DMA idle: low five bits clear, direct-copy ready bit set. */
    g_assert_cmphex(qtest_readl(qts, REG_DMA_BUSY) & 0x1f, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DIRECT_COPY_READY), ==, 0x100);

    qtest_quit(qts);
}

/*
 * The generic widget-header control register every crosstalk widget carries
 * at this offset (docs/datasheets/xbow.md documents Crossbow's own copy,
 * "Widget-zero status, control and interrupt destination"). SI Viz's own
 * control bit assignments were never captured, so this holds the device to
 * plain storage -- readable, writable, reset clear -- rather than either
 * inventing bit semantics or routing a universal generic-header register
 * through the SI-specific unsupported-access path. A live boot against this
 * exact machine reached this offset during the PROM's own widget walk
 * (docs/plans/si-graphics-widget-wiring-handoff-20260916.md on the Origami
 * side); before this register existed, that access fell through to the
 * sticky unsupported-access error and raised a spurious interrupt.
 */
static void test_widget_control_is_plain_storage(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x0000300f);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x0000300f);
    /* Not an SI-specific access: it must not raise the sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x00003000);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x00003000);

    qtest_quit(qts);
}

/*
 * The retained IP27 PROM's own local-IO widget-discovery step: it writes a
 * step-table argument word here for every widget it finds, then polls this
 * same offset for bit 0x2 before continuing. Confirmed live: a guest boot
 * against this exact machine hung forever in "Discovering local IO" before
 * this offset was decoded, and completed it immediately once it was (see
 * docs/plans/si-graphics-widget-wiring-handoff-20260916.md on the Origami
 * side for the full disassembly-backed account).
 */
static void test_generic_bringup_step(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_BRINGUP_STEP, (520u << 10) | (65u << 2));
    g_assert_cmphex(qtest_readl(qts, REG_BRINGUP_STEP) & 0x2, ==, 0x2);

    qtest_quit(qts);
}

/*
 * The kernel driver's own reset routine polls this offset's low five bits,
 * waiting for ((bits + 1) >> 1) < 8 before it will continue -- i.e. the
 * field settling to 14 or below. The value this offset returned before,
 * 31 << 4, fixes those bits at 16, which never satisfies that test: a live
 * boot against this exact machine hung forever inside the driver's reset
 * routine before this was fixed, only reachable at all once a kernel with
 * that driver code actually linked in existed (see the Origami handoff for
 * the retained-media install and kernel rebuild that first exercised it).
 */
static void test_bus_status_reads_settled(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmpuint(((qtest_readl(qts, REG_BUS_STATUS) & 0x1f) + 1) >> 1, <, 8);

    qtest_quit(qts);
}

/*
 * Two registers the reset routine writes once it observes REG_BUS_STATUS
 * settled, live-traced through the same boot: REG_RESET_ARG (a second word
 * next to the existing REG_PREFIX register) and REG_RESET_CONTROL (in the
 * display/cursor register block). Neither field's bit meaning was captured
 * -- only that this driver writes them once during reset and expects the
 * access to succeed -- so both hold the device to plain storage, the same
 * treatment already given to REG_WIDGET_CONTROL above.
 */
static void test_reset_registers_are_plain_storage(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0);
    qtest_writel(qts, REG_RESET_ARG, 0x765b900f);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0x765b900f);

    /*
     * REG_RESET_CONTROL's write side is still plain opaque storage, but
     * its read side no longer echoes it -- see
     * test_board_inventory_registers, which covers the fixed population
     * code the read side now reports.
     */
    qtest_writel(qts, REG_RESET_CONTROL, 0x40000000);
    /* Not SI-specific accesses: neither must raise the sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}

/*
 * The kernel driver's own I2C device probe, live-traced through the same
 * boot once the reset hang above was fixed: it writes a strobe byte to
 * REG_I2C_STROBE, writes a test byte (0x55, then 0xaa if that fails) to
 * REG_I2C_SCRATCH, strobes REG_I2C_STROBE again, and reads REG_I2C_SCRATCH
 * back, concluding a device answers only if the byte read matches the byte
 * just written -- a write/read-back loopback test, not a real I2C exchange.
 * Both registers are byte-wide accesses; this device's own .valid width
 * never permitted anything narrower than 4 bytes before, so the memory core
 * rejected every one of these accesses outright and MIPS turned that
 * rejection into a real bus-error exception -- confirmed live as a "Data
 * Bus Error" kernel panic at this exact offset, immediately after the reset
 * hang above was fixed and the boot reached this code for the first time.
 */
static void test_i2c_probe_loopback(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    qtest_writeb(qts, REG_I2C_SCRATCH, 0x55);
    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0x55);
    /* Not an SI-specific access: it must not raise the sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeb(qts, REG_I2C_SCRATCH, 0xaa);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0xaa);

    qtest_quit(qts);
}

/*
 * The kernel driver's own RE/PP sync handshake, live-traced through the
 * same boot once the I2C probe above stopped blocking it: it writes a
 * channel-select word to REG_REPP_SYNC, writes status bytes to the four
 * registers above it (REG_REPP_STATUS_1/3/4 and REG_SYNC_STATUS, in a
 * different register block), then reads REG_REPP_SYNC back, requiring bits
 * 3:0 and bits 11:8 of the result to agree with each other and equal 1
 * before it continues -- otherwise it retries the whole handshake forever.
 * No value the driver actually writes there has matching nibbles (traced:
 * 0x50214, 0x70214, 0x653, 0x10214), so this device reports the handshake
 * already satisfied rather than echoing any of them back; see the header
 * comment on SGI_MGRAS_REPP_SYNC for the manual and topology evidence this
 * approximation rests on.
 *
 * The fourth status write in this same handshake lands on the offset this
 * device already treats as REG_CMAP_INDEX -- a real, working register from
 * earlier in this device's history, set only by a write to 0x070c30 and
 * merely read back here -- so the handshake's own write to it must be
 * accepted without disturbing whatever REG_CMAP_INDEX already held.
 */
static void test_repp_sync_handshake(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_REPP_SYNC) & 0x00000f0f, ==,
                     0x00000101);

    /* A real 2-byte access, matching MgrasSetColors' own sh instruction --
     * see test_cmap_index_addresses_the_right_slot for why this matters. */
    qtest_writew(qts, SI_REG(0x070c30), 7);
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_INDEX), ==, 7);

    qtest_writel(qts, REG_REPP_STATUS_1, 0x50214);
    qtest_writel(qts, REG_CMAP_INDEX, 0x70214);
    qtest_writel(qts, REG_REPP_STATUS_3, 0x653);
    qtest_writel(qts, REG_REPP_STATUS_4, 0x10214);
    qtest_writel(qts, REG_SYNC_STATUS, 1);
    qtest_writel(qts, REG_REPP_SYNC, 0x50214);
    /* Not SI-specific accesses: none may raise the sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_1), ==, 0x50214);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_3), ==, 0x653);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_4), ==, 0x10214);
    g_assert_cmphex(qtest_readl(qts, REG_SYNC_STATUS), ==, 1);
    /* The handshake's write to this offset must not disturb cmap_index. */
    g_assert_cmphex(qtest_readl(qts, REG_CMAP_INDEX), ==, 7);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_SYNC) & 0x00000f0f, ==,
                     0x00000101);

    qtest_quit(qts);
}

/*
 * The unrolled poll the same boot reaches right after the REPP handshake
 * completes, live-traced with GDB: `lw a0,0(s0); andi a0,a0,0x2; bnez
 * a0,<continue>`, against this same register. This device's first commit
 * returned a plain nonzero value here with no evidence behind the exact
 * bits, since nothing had ever exercised the consumer; that value's bit
 * 0x2 was clear, so the poll spun forever the first time real driver code
 * reached it. See the header comment on the 0x020000 case for why bit 0x2
 * is what the healthy path reports now.
 */
static void test_hq_status_bringup_bit(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);

    qtest_quit(qts);
}

/*
 * The same word's second poll bit: mgras_hq4_hiwater_intr's own short
 * local poll (called from MgrasPcxSwap during a buffer swap), live-traced
 * from the driver object pulled off the retained disk, requiring bit 0x10
 * set before it will treat the FIFO as healthy -- separate from and in
 * addition to bit 0x2's own bring-up consumer above. Before this bit was
 * modelled, that poll always exhausted its ten retries and fell into the
 * same watchdog-arming fallback MgrasFlushHQ3Poll's own timeout does
 * (REG_HQ_ENABLE's bit SI_HQ_ENABLE_LOWATER), on every call, regardless of
 * REG_FIFO_STATUS's own fix.
 */
static void test_hq_status_hiwater_poll_bit(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x10, ==, 0x10);

    qtest_quit(qts);
}

/*
 * CONTRACT CHANGE, needs independent review (see the read case's own
 * comment on this offset in hw/misc/sgi-mgras.c): this register used to
 * report the device's sticky error flag (bit 31) instead of its own
 * healthy bits whenever that flag was set, from any cause anywhere on
 * the device -- so one unrelated unsupported access could make every
 * later, unrelated FIFO status poll falsely report "busy". Root-caused
 * live as part of the SGI_MGRAS_SCHED_ARG_4 (0x040070) investigation
 * (docs/plans/si-graphics-widget-wiring-handoff-20260916.md, "gfxinit
 * clears GE download live, then MgrasCtxsw timeout"). This device
 * already has an independent way to surface "an error occurred" -- the
 * sticky flag's own pending source and widget-error interrupt delivery,
 * introspectable here via x-test-error -- so this register now reports
 * its own healthy bits unconditionally.
 */
static void test_hq_status_independent_of_sticky_error(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);

    /* 0x090000: an offset this device has no case for, so it takes the
     * generic unsupported-access path and latches the sticky error flag. */
    qtest_writel(qts, SI_REG(0x090000), 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 1);

    /* The FIFO status register still reports its own healthy bits, not
     * the unrelated sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x12);

    qtest_quit(qts);
}

/*
 * A colour-lookup-table load reached once the REPP handshake and the HQ
 * status bring-up bit above stopped blocking it: a bounded, index-checked
 * loop that writes a computed word to REG_DAC_LUT_INDEX on every pass, and
 * a byte each to REG_DAC_LUT_DATA_1/2 depending on which channel the pass
 * targets, plus a fourth register (REG_DAC_CONFIG) written once just
 * before the loop starts. None of the four is ever read back, so all four
 * store opaque state -- this loop alone accounted for most of the
 * unsupported-access traffic this device previously logged during a full
 * boot (roughly three writes per table entry, 256 entries).
 */
static void test_dac_lut_load(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_DAC_CONFIG, 0x1c);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_CONFIG), ==, 0x1c);

    qtest_writel(qts, REG_DAC_LUT_INDEX, 0x7300);
    qtest_writel(qts, REG_DAC_LUT_DATA_1, 0xff);
    qtest_writel(qts, REG_DAC_LUT_DATA_2, 0x01);
    /* Not SI-specific accesses: none may raise the sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_INDEX), ==, 0x7300);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_DATA_1), ==, 0xff);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_DATA_2), ==, 0x01);

    qtest_quit(qts);
}

/*
 * Two write-path fixes found once the DAC load above stopped blocking
 * board bring-up. First, the kernel driver's own reset routine writes
 * zero to REG_HQ_STATUS before polling it: with no write case there, that
 * clearing write took the sticky unsupported-access path itself, which
 * poisoned the very read the driver was about to poll -- the driver's own
 * "clear my status" write was what broke its own next read. Second, board
 * bring-up code -- unrelated to the real context/command protocol
 * REG_CONTEXT_COMMAND otherwise enforces -- writes a constant doubleword
 * there before the guest ever selects an HQ entry; accepting that
 * specific case silently avoids raising the sticky error over traffic
 * that has nothing to do with the protocol, while a genuine mid-protocol
 * command submitted before this device reports ready is still flagged.
 *
 * A third fix in this same pass, the SGI_MGRAS_DAC_LUT_DATA_1_LOW
 * carve-out in si_write for a genuinely misaligned 4-byte write live-
 * traced one byte into REG_DAC_LUT_DATA_1's own word, is not exercised
 * here: qtest_writel's own write path enforces natural alignment before
 * the device ever sees the access, so it cannot reproduce the misaligned
 * store a real MIPS swr/swl instruction produced in the field. That fix
 * is verified live instead (see the handoff doc), not by this suite.
 */
static void test_write_path_fixes_dont_raise_the_sticky_error(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_HQ_STATUS, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);

    /* An entry has not been selected yet: this 8-byte write is accepted. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0x482b0400000000ULL);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}

/*
 * A handful of one-shot or small write-only registers found across
 * several distinct board bring-up spots in the same live boot, in
 * register blocks the REPP handshake and DAC load above do not otherwise
 * touch. None is ever read back except as plain storage, so each stores
 * opaque state; REG_BRINGUP_POLL is the one exception, a read-only status
 * poll requiring bit 0x8 set, traced directly from its own retry loop.
 */
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
    /* None of these are SI-specific accesses: none may raise the error. */
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

/*
 * Live-traced polling this register with a 1-byte `lbu` at its own base
 * address, not a 4-byte load: `lbu t0,...; andi t0,t0,0x8; bnez t0,
 * <continue>`. A big-endian 1-byte read at a register's base address
 * extracts the most significant byte of the 32-bit value the memory
 * core widens it from, so the bit this poll checks has to be stored
 * there, not in the low byte a naive 0x08 return would put it in.
 */
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

    /* A later word at the same offset replaces the earlier one. */
    qtest_writel(qts, REG_HQ_UPLOAD, 0x00445566);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD), ==, 0x00445566);

    qtest_quit(qts);
}

/*
 * The GE11 microcode load-and-verify sequence, root-caused by disassembling
 * the retained disk's own rebuilt kernel (MgrasDownloadGE11Ucode,
 * docs/plans/si-graphics-widget-wiring-handoff-20260916.md, "GE microcode
 * download failure"). A control write of 0x200000 starts a load at the
 * first-pass (even) alignment; data-port writes append flat microcode
 * words w0..w5. A control write of 0x40038 paired with a data write of
 * 0xd1 right after is a register-select/value pair the driver sends next,
 * not microcode -- it must not land in the buffer and must not raise the
 * upload-overflow sticky error. Re-arming with 0x200000 and rewinding the
 * read pointer (bit 31) replays the first pass: each verify step reads the
 * discard port (GE_READ_2) then the compare port (GE_READ_1), and the
 * k-th compare-port read yields flat word w[2k]. Arming with 0x200001
 * (odd alignment) instead yields w[2k-1] -- w[-1] reads as 0 on the first
 * read of that pass. The discard-port values asserted below are this
 * device's own self-consistent approximation (the driver never checks
 * them), not confirmed real-hardware evidence -- see the GE port header
 * comment in include/hw/misc/sgi-mgras.h.
 */
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

    /* Register-select/value pair: not microcode, no overflow error. */
    qtest_writel(qts, REG_GE_CTRL_A, 0x40038);
    qtest_writel(qts, REG_GE_DATA_A, 0xd1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    /* First pass: even alignment, compare port yields w[2k]. */
    qtest_writel(qts, REG_GE_CTRL_A, 0x200000);
    qtest_writel(qts, REG_GE_CTRL_A, 0x80000002);
    for (int k = 0; k < 3; k++) {
        g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, w[2 * k + 1]);
        g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, w[2 * k]);
    }

    /* Second pass: odd alignment, compare port yields w[2k-1]; w[-1] is 0. */
    qtest_writel(qts, REG_GE_CTRL_A, 0x200001);
    qtest_writel(qts, REG_GE_CTRL_A, 0x80000004);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, w[0]);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0);
    for (int k = 1; k < 3; k++) {
        g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, w[2 * k]);
        g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, w[2 * k - 1]);
    }
    /* Fourth pair: the discard port runs past the buffer too. */
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_2), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, w[5]);

    qtest_quit(qts);
}

/*
 * The two engine slots keep independent buffers, write/read pointers and
 * alignment state -- interleaving a full load-and-arm sequence on slot B
 * between slot A's own load and its readback must not perturb slot A's
 * compare-port sequence.
 */
static void test_ge_upload_slots_do_not_cross_contaminate(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /*
     * Even alignment maps the k-th compare-port read to flat index 2k, so
     * two consecutive same-pass compare reads need four flat words each
     * (indices 0 and 2); the odd flat indices are discard-port filler,
     * unused by these assertions.
     */
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

    /* Slot A's own compare-port sequence is unaffected by slot B's load
     * and first readback above. */
    qtest_writel(qts, REG_GE_CTRL_A, 0x80000004);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0xaaaa0000);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0xaaaa0001);

    /* Reselecting slot B without rearming (a control write with neither
     * the load pattern nor bit 31 set) resumes its own read pointer
     * exactly where it left off -- unaffected by slot A's reads above. */
    qtest_writel(qts, REG_GE_CTRL_B, 0);
    g_assert_cmphex(qtest_readl(qts, REG_GE_READ_1), ==, 0xbbbb0001);

    qtest_quit(qts);
}

/* Re-arming readback (bit 31) rewinds the read pointer without touching
 * the loaded data: reading, rewinding and reading again returns the same
 * word both times. */
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

/*
 * MgrasDownloadGE11Ucode reads 0x011000 back once during the same
 * sequence; this offset already stores whatever the guest last wrote as
 * the entry-selection-gating prefix state (see REG_PREFIX's other uses),
 * so a plain write-then-read-back must round-trip.
 */
static void test_prefix_register_reads_back(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_PREFIX, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_PREFIX), ==, 0x12345678);

    qtest_quit(qts);
}

/*
 * MgrasCtxsw rewrites this offset on every context switch, after entry
 * selection, as part of its own ordinary per-context HQ-register
 * save/restore -- live-traced against a real MgrasStart hang this
 * device's old post-selection rejection caused (see the 0x011000 row in
 * docs/datasheets/mgras-hq4.md): the rejection poisoned the whole
 * context transaction (protocol went to 0xff), so the following 63-word
 * context payload was rejected wholesale ("context write in invalid
 * state") and the switch never completed. A rewrite after entry
 * selection must not raise the sticky error, and the context
 * transaction that follows must still complete.
 */
static void test_prefix_rewrite_after_entry_selection_does_not_poison_context(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==,
                     SI_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);

    /* MgrasCtxsw's per-context save/restore rewrite, after selection. */
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

    /* No prefix write yet: the entry write must be refused. */
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

    /* Not the token 0xf400: the transaction must stall, not complete. */
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), !=, 0xf400);

    qtest_quit(qts);
}

static void test_pending_mask_and_reassert(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /* Masked: the prefix/entry sequence still latches pending... */
    qtest_writel(qts, REG_IRQ_MASK, 0);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==,
                     SI_PENDING_FLAGS);

    /* ...and re-enabling a still-pending bit's mask must assert it again,
     * observable here only as the pending bit surviving the mask flip. */
    qtest_writel(qts, REG_IRQ_MASK, SI_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==,
                     SI_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_FLAGS, ==, 0);

    qtest_quit(qts);
}

/*
 * Full delivery through Crossbow's own port-to-port routing to a live Hub
 * register is a separate, harder-to-pin-down claim about hw/misc/sgi-xbow.c
 * internals this pass did not settle with confidence. What these tests hold
 * sgi-mgras itself to is its own half of the contract: composing the
 * documented wire form (widget field, vector, address; see the header
 * comment on SGI_MGRAS_HQ_VECTOR_DEST_UPPER, si_deliver_hq_role and
 * si_deliver_widget_error) and issuing exactly one send per role per
 * pending&mask transition, from each role's own destination and vector.
 * x-test-last-interrupt-addr/-value are host-only introspection recording
 * what was last composed, independent of whether the switch actually
 * routed it anywhere.
 *
 * Each test below deliberately programs the *other* destination pair with
 * a different widget/vector than the one it expects, so a regression back
 * to the pre-fix behavior -- every role delivered through the generic
 * widget-error pair's vector, the actual cause of the FIFO-watchdog panic
 * this device used to hit on every guest that opened this board -- shows
 * up as a wrong address or vector, not just a missing packet.
 */
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

    /* Bit 8 raises, low byte carries the general role's own vector
     * (REG_HQ_VECTORS bits 15:8), from the HQ vector-dest pair -- not the
     * generic widget-error pair's vector or address. */
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, general_vector | 0x100);

    /* Acknowledging the pending bit is itself a pending&mask level
     * transition (asserted -> deasserted), so it sends its own packet,
     * this time without the raise bit. */
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, general_vector);

    qtest_quit(qts);
}

/*
 * The retrace role (MgrasRetraceHandler): delivers via the same HQ
 * vector-dest pair as general and low-water, from its own byte of
 * REG_HQ_VECTORS (bits 23:16), gated by REG_IRQ_MASK the same way general
 * is (unlike low-water, which bypasses it -- see si_update_irq's own
 * comment). si_retrace only fires once the display is enabled (VC3 index
 * 0x1e bit 0x4, MgrasEnableVideoTiming -- the existing, separately
 * evidenced gate this test reuses unchanged; see the header comment on
 * SGI_MGRAS_VC3_INDEXDATA).
 */
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

    /* si_retrace re-arms itself on this same period (sgi-mgras.c); one
     * step is enough to fire it once. */
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 60);

    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_RETRACE, ==,
                     SI_PENDING_RETRACE);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, retrace_vector | 0x100);

    qtest_quit(qts);
}

/*
 * The widget-error role (MgrasHQ4ErrInterrupt): unchanged from this
 * device's original, single delivery path -- the generic widget
 * destination pair (REG_INT_DEST_UPPER/LOWER, aliased at
 * REG_INT_DEST_UPPER_ALT/LOWER_ALT), vector in its own top byte -- see
 * si_deliver_widget_error. Before this device modelled per-role vectors,
 * this was the *only* path every role's packet ever took (confirmed live:
 * "HQ4 widget err - 0x0," in every retained panicking console log), so
 * this test's own regression check runs the other direction from the
 * three above: the HQ vector-dest pair is programmed with a different
 * vector and destination, proving the error role does not deliver there
 * instead.
 */
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

    /* Only the error role unmasked: si_enter_context's own FLAGS/CONTEXT
     * transitions must not deliver a general packet here. */
    qtest_writel(qts, REG_IRQ_MASK, SI_PENDING_ERROR);
    si_enter_context(qts);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, 0);

    /*
     * A write to a genuinely unmapped offset: si_error's own sticky-error
     * path. Not an invalid command selector -- si_command no longer
     * rejects any selector, see its own header comment in sgi-mgras.c.
     * Checked through x-test-error, not REG_HQ_STATUS's own bit 31: that
     * bit no longer folds the sticky error in (see the 0x020000 read
     * case's own contract-change comment), so this device's dedicated
     * error introspection is the right way to confirm the flag latched.
     */
    qtest_writel(qts, SI_REG(0x000010), 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-error"), ==, 1);

    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, error_vector | 0x100);

    qtest_quit(qts);
}

/*
 * si_command now applies every state and execute record to the persistent
 * register file immediately, with no modelled queueing depth (docs/
 * designs/2026-09-17-si-viz-pixel-output-design.md's ordered plan, step 1;
 * see also the header comment on REG_FIFO_STATUS's case in sgi-mgras.c).
 * Occupancy is therefore always zero, both through the real MMIO register
 * and internally -- overflow is not reachable the way it used to be, so
 * this replaces the old test that filled a modelled 256-record FIFO and
 * checked the 257th record was refused.
 */
static void test_fifo_occupancy_always_drained(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    /* Many more state records than the old modelled FIFO depth, none of
     * them naming an execute register. */
    for (int i = 0; i < 300; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     SI_COMMAND(REG_FILL_CLIP_START, i));
        g_assert_cmphex(qtest_readl(qts, REG_FIFO_STATUS) & 0xff, ==, 0);
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}

/*
 * Solid fill against the real public register grammar: state records for
 * the block rectangle and packed colour, then an execute-form write of
 * BLOCKXYENDI triggers the fill (design doc, ordered plan step 2). A
 * second fill over the same rectangle with an XOR logic op flips the same
 * pixels back to zero, exercising PP1FILLMODE's top nibble and the
 * persistent register file's own headline feature: colour and the block
 * rectangle are not re-supplied, they are read from state set once and
 * reused.
 */
static void test_direct_fill_and_reset(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    /* Solid fill: a 4x4 rectangle at (10,10)-(13,13), default (copy)
     * logic op from this device's own reset default. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));

    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0x00112233);
    g_assert_cmphex(si_test_pixel(qts, 13, 13), ==, 0x00112233);
    /* Outside the filled rectangle stays untouched. */
    g_assert_cmphex(si_test_pixel(qts, 20, 11), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 9, 9), ==, 0);

    /* An XOR fill over the same rectangle, reusing the colour and block
     * rectangle already in the persistent register file, flips those
     * pixels back to zero. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PP1FILLMODE, SI_LOGIC_OP_XOR << 26));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));
    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0);

    /* Reset returns the device to its documented cleared state. */
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(qtest_readl(qts, REG_PENDING), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ENTRY), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0);

    qtest_quit(qts);
}

/*
 * The plane mask a fill's own visible output actually depends on is
 * COLORMASKLSBSA, not COLORMASKMSBS -- live-traced against OpenBSD's own
 * public driver (sys/arch/sgi/dev/impact.c, "RSS initialization"), which
 * sets COLORMASKMSBS to 0 and COLORMASKLSBSA to 0xffffff once, at
 * attach, and leaves both alone before every later fill. Reproduces
 * that exact pair -- the one a real driver actually leaves live -- and
 * checks the fill still paints, then checks COLORMASKLSBSA on its own
 * genuinely gates the write (closing it back to 0 makes the same fill a
 * no-op), confirming which register this device's own mask reads from.
 */
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

    /* Closing COLORMASKLSBSA, with COLORMASKMSBS untouched, makes the
     * identical fill a no-op -- confirming LSBSA is the register that
     * actually gates a 24-bit colour write, not MSBS. */
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

/*
 * IR_ALIAS (register 0x045), reached through the direct-record grammar's
 * own execute-form write: before this pass, si_command_execute had no
 * case for it at all and it fell to the logged default, painting nothing.
 * OpenBSD's public driver (impact.c) issues IR_ALIAS as the execute
 * trigger nearly every 2D primitive ends with, and a live trace of a real
 * boot-to-desktop session found 382 real executes through this exact
 * register that this device was silently dropping -- see si_draw_ir_
 * alias_fill's own header comment in hw/misc/sgi-mgras.c for the evidence.
 *
 * Colour comes from BKGRD_RG, not PACKEDCOLOR: the same cited trace found
 * PACKEDCOLOR reading 0x000000 at every one of those 382 executes while
 * BKGRD_RG carried real, varying values, the opposite of BLOCKXYENDI's
 * own real traffic. This also exercises si_fill_rect's own corner-
 * normalizing behaviour -- BLOCKXYSTARTI's Y is larger than BLOCKXYENDI's
 * here, the reverse of BLOCKXYENDI's own convention, matching 148 of the
 * 382 real IR_ALIAS executes in the same trace.
 */
static void test_ir_alias_execute(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BKGRD_RG, 0x00aabbcc));
    /* Reversed corners: start row 23, end row 20 -- the convention real
     * IR_ALIAS traffic uses, not BLOCKXYENDI's top-left/bottom-right one.
     * Both corners are plain state writes; IR_ALIAS's own execute-form
     * write carries a small trigger value (0x18, matching real traffic),
     * not a coordinate -- si_draw_ir_alias_fill reads the rectangle from
     * BLOCKXYSTARTI/BLOCKXYENDI the same way si_draw_fill does. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 23u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (13u << 16) | 20u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_IR_ALIAS, 0x18));

    g_assert_cmphex(si_test_pixel(qts, 11, 21), ==, 0x00aabbcc);
    g_assert_cmphex(si_test_pixel(qts, 13, 20), ==, 0x00aabbcc);
    g_assert_cmphex(si_test_pixel(qts, 10, 23), ==, 0x00aabbcc);
    /* Outside the normalized rectangle stays untouched. */
    g_assert_cmphex(si_test_pixel(qts, 20, 21), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 10, 24), ==, 0);

    /* PACKEDCOLOR plays no part in this trigger's own colour: setting it
     * to something else and re-running the fill still paints BKGRD_RG's
     * colour. */
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

/*
 * 1-bit glyph row expansion: the same 8x8 bitmap this test has always
 * used (the design doc's own cited fixture,
 * `si-direct-command-inputs-20260916.json` on `emdash/graphics-sarct`,
 * "opaque-glyph-8x8" -- its own `command_records` are `null` for every
 * operation, so the register-write sequence below is still this pass's
 * own construction, not a captured one), pushed the way disassembling the
 * shipped 2D driver's own glyph-blit routines (mgrasImageGlyphBlt,
 * mgrasPolyGlyphBlt) shows real traffic does: one CHAR_H execute-form
 * write per bitmap row, the row's own byte in bits 31:24 (the top of the
 * payload's real bits 31:16 -- an 8-pixel-wide glyph only uses the top 8
 * of those 16), no CHAR_L involved (si_draw_glyph_row's own header
 * comment in sgi-mgras.c). The fixture's own pixel rule -- "bitmap bit 1
 * selects foreground; bit 0 selects background" -- matches this device's
 * set-bit-is-foreground choice, unchanged by this pass.
 */
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

    /* Row 0, 0x18 = 00011000: pixels 3 and 4 set, the rest background. */
    g_assert_cmphex(si_test_pixel(qts, 40, 40), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 43, 40), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 44, 40), ==, 0x00ffffff);
    /* Row 3, 0x7e = 01111110: both ends background, the middle set. */
    g_assert_cmphex(si_test_pixel(qts, 40, 43), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 41, 43), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 47, 43), ==, 0);
    /* Row 7, 0x00: every pixel is background. */
    g_assert_cmphex(si_test_pixel(qts, 40, 47), ==, 0);
    /* Outside the glyph's own rectangle stays untouched. */
    g_assert_cmphex(si_test_pixel(qts, 50, 40), ==, 0);

    qtest_quit(qts);
}

/*
 * A CHAR_H push only ever carries 16 real pixel bits (bits 31:16 of the
 * 32-bit payload; disassembling the shipped driver's own row-push loop
 * found it always clears bits 15:0 before the store -- si_draw_glyph_row's
 * own header comment). A rectangle wider than 16 pixels still only paints
 * from those 16 bits: pixels past column 15 are never touched by this
 * push, not painted from the (always-zero-in-real-traffic) low half.
 */
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
    /* 24 pixels wide -- wider than the 16 real payload bits. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (103u << 16) | 80u));
    /* Every one of the low 16 bits set, to prove they are never read. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_CHAR_H, 0x8000ffffu));

    /* Pixel 0 (bit 31, set): foreground. Pixels 1-15 (clear): background. */
    g_assert_cmphex(si_test_pixel(qts, 80, 80), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 81, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 95, 80), ==, 0);
    /* Columns 96-103, still inside the block rectangle but past the 16
     * real payload bits, stay background rather than painting from the
     * (all-set) low half. */
    g_assert_cmphex(si_test_pixel(qts, 96, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 103, 80), ==, 0);

    qtest_quit(qts);
}

/*
 * CHAR_L's own execute-form write is not a mono-glyph trigger: disassembling
 * the shipped driver's real glyph-blit routines (mgrasImageGlyphBlt,
 * mgrasPolyGlyphBlt) found CHAR_L's own execute selector computed but never
 * issued on that path, and a live desktop trace confirmed it -- 496 execute
 * hits on CHAR_H, zero on CHAR_L (docs/datasheets/mgras-hq4.md's `0x120400`
 * row). si_command_execute leaves CHAR_L's execute form on its own logged,
 * no-op default case rather than painting anything; this pins that down as
 * a tested contract rather than only a comment (si_draw_glyph_row's own
 * header comment in sgi-mgras.c has the fuller evidence trail, including
 * the separate antialiased path that genuinely does trigger through
 * CHAR_L, which this device does not implement).
 */
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
    /* Logged as unimplemented, not raised as a sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}

/*
 * 32-bit PIO pixel-pair upload: BKGRD_RG/BKGRD_BA reused as a raw pixel
 * pair, the write to BKGRD_BA's own execute form painting both at once and
 * advancing this device's own write cursor (si_draw_pio_pixel_pair's own
 * header comment states which two registers carry PIO traffic is this
 * device's approximation, not a confirmed wire contract). The 4x4 image
 * from the design doc's own cited fixture ("packed-image-4x4-aligned",
 * same file as the glyph test above -- again reachable, but with
 * `command_records: null`, so the register sequence is this pass's own
 * construction), uploaded as eight pixel pairs, checks the cursor wraps a
 * row at the block rectangle's own width. That fixture also flags its own
 * PIO path as `"pio_without_ioctl_0x6d67000b": "unqualified"` -- further,
 * independent evidence that this device's own choice of trigger register
 * for PIO traffic is an approximation, not a captured contract.
 */
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
    /* Outside the image's own rectangle stays untouched. */
    g_assert_cmphex(si_test_pixel(qts, 170, 96), ==, 0);

    qtest_quit(qts);
}

/*
 * The display-surface conversion x-test-display-pixel introspects
 * (si_gfx_update, hw/misc/sgi-mgras.c): black while the VC3's own
 * display-enable bit (index 0x1e bit 2) is clear, regardless of what
 * front[] holds; PACKEDCOLOR's own 0x00BBGGRR converted to
 * PIXMAN_x8r8g8b8's 0x00RRGGBB (B and R swap byte position, G stays put)
 * once enabled; and correctly re-armed by a later, independent fill --
 * not stuck showing whatever the first forced update already converted.
 */
static void test_display_surface_conversion(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    /* Draw before the display is enabled: front[] holds real content, but
     * a real VC3 blanks its output while disabled, so the surface should
     * still read black. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | 13u));
    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0x00112233);
    g_assert_cmphex(si_test_display_pixel(qts, 11, 11), ==, 0);

    /* Enabling the display converts front[]'s 0x00BBGGRR into the
     * surface's 0x00RRGGBB: B (0x11) and R (0x33) swap byte position, G
     * (0x22) stays put. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    g_assert_cmphex(si_test_display_pixel(qts, 11, 11), ==, 0x00332211);
    /* Outside the filled rectangle is still black either way. */
    g_assert_cmphex(si_test_display_pixel(qts, 20, 11), ==, 0);

    /* A second, independent fill re-arms the dirty flag: the surface
     * updates again rather than staying stuck at whatever the first
     * forced update already converted. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00aabbcc));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (31u << 16) | 31u));
    g_assert_cmphex(si_test_display_pixel(qts, 30, 30), ==, 0x00ccbbaa);
    /* The first fill's own pixel is untouched by the second. */
    g_assert_cmphex(si_test_display_pixel(qts, 11, 11), ==, 0x00332211);

    /* Disabling the display blanks the surface again, even though front[]
     * still holds both fills. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x0));
    g_assert_cmphex(si_test_display_pixel(qts, 11, 11), ==, 0);
    g_assert_cmphex(si_test_display_pixel(qts, 30, 30), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 30, 30), ==, 0x00aabbcc);

    qtest_quit(qts);
}

/*
 * PseudoColor scanout: once REG_XMAP_MAIN_MODE names a PseudoColor window
 * (the same 0x601-family value real Xsgi traffic writes there -- see the
 * header comment on SGI_MGRAS_XMAP_MAIN_MODE, include/hw/misc/sgi-mgras.h),
 * front[]'s stored byte is a CMAP index, looked up through the colour map
 * this device already accepted writes for, rather than painted directly
 * into the red channel. A TrueColor window (the driver's own real 0x7xx
 * family, and this device's reset default) keeps the pre-existing direct-
 * RGB scanout unchanged.
 */
static void test_pseudocolor_scanout_looks_up_cmap(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    /* Load CMAP slot 7 with a real colour. */
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 7);
    qtest_writel(qts, REG_CMAP_DATA, 0x00aabbcc);

    /* Paint a pixel whose own low byte is just the index 7 -- the same
     * "index stored where PACKEDCOLOR's own RR byte lives" shape real
     * PseudoColor traffic uses (small integers like 0x07/0x11/0x14). */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_PACKEDCOLOR, 7));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(si_test_pixel(qts, 5, 5), ==, 7);

    /* Before the XMAP mode names a PseudoColor window (this device's
     * TrueColor reset default), scanout is still direct RGB: the raw
     * index paints straight into the red channel. */
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x00070000);

    /* Naming PseudoColor routes the same pixel through the colour map
     * instead -- no new fill, only the mode word changed. */
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x601);
    g_assert_cmphex(si_test_display_pixel(qts, 5, 5), ==, 0x00ccbbaa);

    /* A TrueColor window keeps direct RGB scanout: a pixel holding a
     * real 24-bit colour is unaffected by the colour map. */
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x7a4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (6u << 16) | 6u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (6u << 16) | 6u));
    g_assert_cmphex(si_test_display_pixel(qts, 6, 6), ==, 0x00332211);

    qtest_quit(qts);
}

/*
 * Hardware cursor compositing: si_composite_cursor (hw/misc/sgi-mgras.c)
 * reads the two 32x32 1-bit planes at the cursor-entry pointer (VC3 index
 * 1) out of VC3 SRAM and draws them into the scanout at (cursor_x,
 * cursor_y) only while enabled. Builds a tiny two-pixel sprite: row 0
 * column 0 opaque and colour-selected to the "ink" colour (CMAP slot
 * 0x1c, the slot this device's own live trace ties to the cursor -- see
 * si_composite_cursor's own header comment), row 1 column 1 opaque and
 * colour-selected to the background colour, row 0 column 1 left
 * transparent.
 */
static void test_cursor_composites_when_enabled(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    /* "Ink" colour for CMAP slot 0x1c: pure red in this device's own
     * 0x00BBGGRR packing. */
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, 0x1c);
    qtest_writel(qts, REG_CMAP_DATA, 0x000000ff);

    /* Cursor entry pointer: SRAM halfword address 0x100. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0100));

    /* First plane (transparency mask), two halfwords per row: row 0
     * column 0 opaque, row 1 column 1 opaque, everything else clear. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x8000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x4000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);

    /* Second plane (colour select), 64 halfwords further on: row 0
     * selects the ink colour, row 1 selects the background colour. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0140));
    qtest_writew(qts, REG_VC3_RAM, 0x8000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 50));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 60));

    /* Disabled: the sprite does not appear, even though it is fully
     * programmed. */
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0);

    /* Enabling composites it. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0x00ff0000);
    g_assert_cmphex(si_test_display_pixel(qts, 51, 61), ==, 0x00ffffff);
    /* The masked-off pixel between them is untouched. */
    g_assert_cmphex(si_test_display_pixel(qts, 51, 60), ==, 0);

    /* Disabling removes it again. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0000));
    g_assert_cmphex(si_test_display_pixel(qts, 50, 60), ==, 0);

    qtest_quit(qts);
}

static void test_vc3_sram_round_trip(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /* Index 7 loads the SRAM address pointer; the data port then reads
     * back whatever was stored at that same address, auto-incrementing
     * after each access the same way the load did. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x1234);
    qtest_writew(qts, REG_VC3_RAM, 0x5678);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x0100));
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0x5678);

    /* The address field wraps at the SRAM's own 0x8000-halfword size. */
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

    /* An ordinary register: written through the index/data port, read
     * back through the select-then-data pair. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x06, 0x2468));
    qtest_writeb(qts, REG_VC3_INDEX, 0x06);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 0x2468);

    /* Cursor position (indices 2/3) also updates this device's own
     * named cursor state, unchanged from before this register file
     * existed. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 100));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 200));
    qtest_writeb(qts, REG_VC3_INDEX, 0x02);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 100);
    qtest_writeb(qts, REG_VC3_INDEX, 0x03);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 200);

    /* Index 0x1d bit 1 enables the cursor; index 0x1e bit 2 enables
     * video display -- both were previously driven by a since-removed
     * ad hoc tag check on this same write port. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x0004));

    /* Index 0x1f (config) echoes back what was written, except its own
     * revision bits (6:5) always report "VC3 A", revision 0. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1f, 0x01ff));
    qtest_writeb(qts, REG_VC3_INDEX, 0x1f);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 0x019f);

    qtest_quit(qts);
}

static void test_board_inventory_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /* Fixed population codes for this SI board class: one GE, single-RE,
     * no RB daughterboard; both TRAM fields report absent. Neither
     * echoes a write -- see the header comments on SGI_MGRAS_
     * BOARD_INVENTORY and SGI_MGRAS_RESET_CONTROL. */
    g_assert_cmphex(qtest_readl(qts, REG_BOARD_INVENTORY), ==, 0x21000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
    qtest_writel(qts, REG_RESET_CONTROL, 0xe8000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);

    qtest_quit(qts);
}

static void test_cmap_index_addresses_the_right_slot(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /* Before the width fix, this 2-byte index write always landed in the
     * low byte of a widened 4-byte value, which this device's big-endian
     * access adjustment never populates -- so every index selected slot
     * zero regardless of what the guest asked for. */
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

/*
 * MgrasSetHQ4IntrVectors programs the same interrupt-destination fields the
 * generic widget-header pair already models, but through a second address
 * pair one word below it; both must feed the same destination state.
 */
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

/*
 * MgrasCtxsw and MgrasFlushHQ3 save, restore and read-modify-write these
 * four registers around a context switch. The device only needs to store
 * and echo each faithfully -- the driver does its own read-modify-write in
 * software -- except SGI_MGRAS_HQ_ENABLE_CLEAR (SGI_MGRAS_BRINGUP_ARG_3's
 * offset), which clears bits from SGI_MGRAS_HQ_ENABLE rather than
 * replacing it outright.
 */
static void test_hq_config_and_enable_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_HQ_CONFIG_A, 0x11000f);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_CONFIG_A), ==, 0x11000f);
    qtest_writel(qts, REG_HQ_CONFIG_B, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_CONFIG_B), ==, 0xffffffff);

    qtest_writel(qts, REG_HQ_ENABLE, 0x1f8);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1f8);
    /* The driver's own read-modify-write: (v & ~8) | 0x10. */
    qtest_writel(qts, REG_HQ_ENABLE, (0x1f8 & ~8) | 0x10);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1f0);

    qtest_writel(qts, REG_HQ_ENABLE_CLEAR, 0x10);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1e0);

    /* Not SI-specific accesses: none may raise the sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}

/*
 * The FIFO status/low-water interrupt: MgrasFlushHQ3 and
 * mgras_hq4_hiwater_intr's own timeout fallback both arm the driver's FIFO
 * watchdog by writing bit SI_HQ_ENABLE_LOWATER into REG_HQ_ENABLE
 * (live-traced from both routines), then wait for mgras_hq4_lowater_intr to
 * run; only that interrupt ever clears the bit back out (via
 * REG_HQ_ENABLE_CLEAR). Before this device modelled the interrupt at all,
 * nothing ever ran it, and IRIX panicked its own watchdog roughly 15
 * seconds after any code path (gfxinit, the X server) opened this board.
 * Modelling the pending bit alone was not enough either: this device still
 * delivered every role, this one included, through the generic
 * widget-error destination pair's vector -- which routes to
 * MgrasHQ4ErrInterrupt, a printf-only handler -- so mgras_hq4_lowater_intr
 * still never ran. This test's own regression check programs that pair
 * with a different vector and destination than the HQ vector-dest pair's,
 * so a regression back to the shared path shows up as a wrong address or
 * vector delivered, not just a missing packet. This device tracks no real
 * FIFO occupancy, so it satisfies the wait immediately on the arming write
 * instead: this test holds it to that behavior, not to genuine watermark
 * timing this project has no evidence for.
 *
 * A third bug closed here (docs/plans/si-graphics-widget-wiring-handoff-
 * 20260916.md, "gfxinit clears GE download live, then MgrasCtxsw
 * timeout"): real driver usage never acks this pending source through
 * REG_PENDING_ACK at all -- mgras_hq4_lowater_intr's own disarming write
 * to REG_HQ_ENABLE_CLEAR is the only clear this bit ever gets. Before the
 * fix, that write cleared REG_HQ_ENABLE's own bit but left the pending
 * bit stuck set, so a second, routine arming (MgrasFlushHQ3 arms this bit
 * on every ordinary FIFO wait, not only a timeout) could never raise a
 * second packet: si_update_irq's edge detector never saw pending go
 * false, so it never saw it go true again either. Both watchdogs then
 * expired with nobody ever disarming them. The disarm-then-rearm sequence
 * right after the first delivery below is that exact real sequence,
 * reproduced without going through REG_PENDING_ACK at all.
 */
static void test_fifo_lowater_interrupt(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    const unsigned int lowater_vector = 60;
    const unsigned int error_vector = 61;
    const uint64_t expected_addr =
        ((uint64_t)HQ_WIDGET << 48) | HQ_REG_OFFSET;

    /* The generic widget-error pair: a deliberately different destination
     * and vector than the HQ vector-dest pair below. */
    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS, lowater_vector);

    /*
     * Left masked out of the shared top-level enable on purpose: this
     * source is not gated by REG_IRQ_MASK (see si_update_irq's own
     * comment), only by REG_HQ_ENABLE's own bit.
     */
    qtest_writel(qts, REG_IRQ_MASK, 0);

    /* Arming: a newly-set low-water bit raises the interrupt promptly,
     * carrying low-water's own vector (REG_HQ_VECTORS bits 7:0) from the
     * HQ vector-dest pair -- not the widget-error pair's vector or
     * address. */
    qtest_writel(qts, REG_HQ_ENABLE, SI_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==,
                     SI_PENDING_LOWATER);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector | 0x100);

    /*
     * Disarming with no preceding REG_PENDING_ACK -- the real sequence --
     * must clear the pending bit itself and lower the interrupt. This is
     * the regression check for the bug described above: before the fix,
     * both assertions below failed (the pending bit stayed set and no
     * "lowered" packet was ever sent).
     */
    qtest_writel(qts, REG_HQ_ENABLE_CLEAR, SI_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE) & SI_HQ_ENABLE_LOWATER,
                     ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==, 0);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector);

    /*
     * Arming again after that real disarm must raise a fresh packet --
     * before the fix, this never happened: the stuck pending bit meant
     * si_update_irq's edge detector had nothing to transition from.
     */
    qtest_writel(qts, REG_HQ_ENABLE, SI_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==,
                     SI_PENDING_LOWATER);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, lowater_vector | 0x100);

    /*
     * Re-writing the same already-set bit is not a new arming event, and
     * REG_PENDING_ACK remains a separate, independent way to clear the
     * shared pending register -- both properties this device already had
     * before the fix above, unaffected by it.
     */
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==, 0);
    qtest_writel(qts, REG_HQ_ENABLE, SI_HQ_ENABLE_LOWATER);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & SI_PENDING_LOWATER, ==, 0);

    qtest_quit(qts);
}

/*
 * The HQ4-specific interrupt-vector triple MgrasSetHQ4IntrVectors writes
 * alongside the generic destination pair: plain register-storage round
 * trip coverage. See the header comment on SGI_MGRAS_HQ_VECTOR_DEST_UPPER
 * for which byte of REG_HQ_VECTORS serves which interrupt role, and
 * test_general_interrupt_vector/test_retrace_interrupt_vector/
 * test_fifo_lowater_interrupt for that behavior.
 */
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

/*
 * SGI_MGRAS_SCHED_ARG_4 (0x040070): MgrasInitialize's own flow-control
 * credit word, computed as (0x11 << 3) | (board_byte << 8) | 0x1000 and
 * written once during setup, never read back. Root-caused live as the
 * trigger for the gfxinit CTXSW/CFIFO timeout (docs/plans/
 * si-graphics-widget-wiring-handoff-20260916.md, "gfxinit clears GE
 * download live, then MgrasCtxsw timeout"): with no case here, the write
 * took the generic unsupported-access path and latched this device's
 * sticky error flag, which poisoned every later REG_HQ_STATUS read. A
 * plain write/read round trip that raises no sticky error is the
 * regression check for this specific fix.
 */
static void test_sched_arg_4_round_trip(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    qtest_writel(qts, REG_SCHED_ARG_4, 0x1885);
    g_assert_cmphex(qtest_readl(qts, REG_SCHED_ARG_4), ==, 0x1885);
    /* Not an SI-specific access: it may not raise the sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}

/*
 * MgrasInitHQ writes FIFO low/high-water thresholds across this small
 * range once at start-of-day; none is ever read back by the driver, but
 * this device stores and echoes the whole range like the larger board-
 * configuration table.
 */
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

/*
 * CONTRACT CHANGE, needs independent review (see the header comment on
 * SGI_MGRAS_HQ_FLAGS, include/hw/misc/sgi-mgras.h): REG_HQ_FLAGS/
 * REG_HQ_FLAGS_CLEAR were previously modelled as two independent opaque
 * scratch registers. Disassembling the DDX's own fence sequence alongside
 * the kernel driver's read/clear sites settled the real mechanism -- a
 * write to REG_HQ_FLAGS ORs bits in, a write to REG_HQ_FLAGS_CLEAR AND-NOTs
 * them back out, and both offsets read the same underlying flag word.
 */
static void test_hq_flags_set_and_clear(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0);

    /* A write ORs bits in rather than replacing the word. */
    qtest_writel(qts, REG_HQ_FLAGS, 0x100000);
    qtest_writel(qts, REG_HQ_FLAGS, 0x000010);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0x100010);
    /* Both offsets read back the same underlying word. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS_CLEAR), ==, 0x100010);

    /* A write to the clear alias AND-NOTs only the bits it names. */
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, 0x100000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS), ==, 0x000010);

    qtest_quit(qts);
}

/*
 * The regression this fix closes: Xsgi's mgrasCopyPixels/mgrasDrawPixels
 * fence sequence spun forever polling REG_HQ_FLAGS bit SI_HQ_FLAG_FENCE,
 * since nothing on this device ever set it -- confirmed live as the exact
 * PC and poll address Xsgi itself stalls on (gdb PC sampling against a
 * running guest, see the SI graphics bring-up notes). The fix gives the
 * command-FIFO write path at REG_CONTEXT_COMMAND real token-header parsing
 * and dispatches the one header the fence sequence is evidenced to depend
 * on: class 0x000e reg 0x04, no payload (SI_FENCE_TOKEN, the second push's
 * low word). The first push and the second push's high word are
 * unrecognized traffic this device only needs to accept, not act on.
 */
static void test_hq_flags_fence_token_sets_bit(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==, 0);

    /* The exact push sequence mgrasCopyPixels/mgrasDrawPixels issue. */
    qtest_writel(qts, REG_CONTEXT_COMMAND, SI_FENCE_PUSH_1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_FENCE_PUSH_2);
    /* Not SI-specific traffic in the sense of a protocol error: neither
     * push may raise the sticky error. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                     SI_HQ_FLAG_FENCE);

    /* The same poll's own disarm: a clear write of just that bit. */
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, SI_HQ_FLAG_FENCE);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==, 0);

    qtest_quit(qts);
}

/*
 * The token alone, isolated from the rest of the push sequence: a bare
 * 64-bit write whose low word is SI_FENCE_TOKEN dispatches the same way
 * regardless of what else accompanies it in a real fence sequence.
 */
static void test_hq_flags_fence_token_isolated(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & SI_HQ_FLAG_FENCE, ==,
                     SI_HQ_FLAG_FENCE);

    qtest_quit(qts);
}

/*
 * MgrasStart submits two 64-bit HQ-local configuration words (GE and RE
 * population counts) around the context switch, with a zero unit field in
 * the high half rather than a real command selector, and the driver also
 * submits HQ-local DMA words at this same offset (0x000c0004, 0x000a0204,
 * 0x00080004 in the record's high half) -- none of these match the
 * corrected state/execute selector shape (si_command's own header
 * comment), so si_command now accepts every one of them, and any other
 * non-matching selector, as opaque traffic rather than rejecting it. This
 * device's direct-command decoder no longer has an "unknown command
 * selector" error path at all.
 */
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

    /* A selector that would have been out of range under the old decoder
     * is opaque traffic now too, not rejected. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, ((uint64_t)0x00000001 << 34) | 1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_quit(qts);
}

/*
 * MgrasStart and MgrasCtxsw submit further 4-byte FIFO words to
 * REG_CONTEXT_COMMAND after the context switch has already completed
 * (0xf200, 0x80000008, 0x40000, 0x111, 0x3700, zero, and others); these
 * must be accepted as opaque post-completion traffic, not flagged as a
 * protocol violation the way a write before the first acknowledgement
 * still is.
 */
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
    /* The completion token itself still reads back afterward. */
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);

    qtest_quit(qts);
}

/*
 * State-write command-FIFO token headers (class 0x18/0x19) feed the same
 * persistent register file the direct-record grammar's own state writes
 * do (si_rss_write, hw/misc/sgi-mgras.c): setting up a fill entirely
 * through token headers, then triggering it with the direct-record
 * grammar's own execute form, proves the two wire encodings share state
 * rather than each keeping its own copy.
 */
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

    /* The fill itself still triggers through the direct-record grammar's
     * own execute form, reading state the token headers above just set. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (61u << 16) | 61u));
    g_assert_cmphex(si_test_pixel(qts, 60, 60), ==, 0x00223344);
    g_assert_cmphex(si_test_pixel(qts, 61, 61), ==, 0x00223344);
    g_assert_cmphex(si_test_pixel(qts, 59, 59), ==, 0);

    qtest_quit(qts);
}

/*
 * State-write headers for registers this device names but does not yet
 * consume in any execute implementation (see the header comment on
 * SI_CMD_HDR_CLASS_STATE) -- accepted and stored generically, not flagged,
 * the same "traffic this device doesn't act on yet is not an error"
 * treatment test_command_accepts_hq_config_words already covers for the
 * direct-record grammar's own unmatched selectors.
 */
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

/*
 * Single-pixel plot: RE_POINT's own execute-form token header (class 0x1c
 * reg 0x040) paints exactly one pixel, at BLOCKXYSTARTI, in the current
 * colour -- si_draw_point's own header comment states the bounded,
 * single-point scope this device gives the trigger.
 */
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
    /* Only the one pixel -- not a filled rectangle. */
    g_assert_cmphex(si_test_pixel(qts, 71, 80), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 70, 81), ==, 0);

    qtest_quit(qts);
}

/*
 * Line draw: RE_LINE's own execute-form token header (class 0x1c reg
 * 0x041) draws a Bresenham line from BLOCKXYSTARTI to BLOCKXYENDI in the
 * current colour -- si_draw_line's own header comment states this is a
 * bounded choice of stepping algorithm, not a claim about real hardware's
 * exact pixels.
 */
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

    /* The diagonal's own endpoints and midpoint. */
    g_assert_cmphex(si_test_pixel(qts, 100, 100), ==, 0x00112233);
    g_assert_cmphex(si_test_pixel(qts, 102, 102), ==, 0x00112233);
    g_assert_cmphex(si_test_pixel(qts, 105, 105), ==, 0x00112233);
    /* Off the diagonal, inside the same bounding box, stays untouched. */
    g_assert_cmphex(si_test_pixel(qts, 100, 105), ==, 0);

    qtest_quit(qts);
}

/*
 * CHAR_H's own execute-form token header (class 0x1c reg 0x070) reaches
 * the same si_draw_glyph_row primitive the direct-record form does
 * (si_command_execute's shared case), painting the row this same header's
 * own state-write counterpart (class 0x18/0x19) just stored into CHAR_H's
 * register-file slot -- proving the two wire encodings share one glyph
 * contract, not that CHAR_L plays any part in it (CHAR_L is not a
 * mono-glyph trigger at all; see si_draw_glyph_row's own header comment
 * and test_char_l_execute_is_not_a_glyph_trigger above).
 */
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

    /* 0x18 = 00011000: pixels 3 and 4 set, the rest background. */
    g_assert_cmphex(si_test_pixel(qts, 200, 200), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 203, 200), ==, 0x00ffffff);
    g_assert_cmphex(si_test_pixel(qts, 204, 200), ==, 0x00ffffff);

    qtest_quit(qts);
}

/*
 * Framebuffer block copy: RE_BLOCKCOPY's own execute-form token header
 * (class 0x1c reg 0x045) declares a six-word payload (0x18 bytes) instead
 * of the usual zero -- si_draw_block_copy's own header comment gives the
 * evidence and the bounded scope (a plain in-buffer rectangle copy, source
 * origin from the payload's own first word). Spreading the header and its
 * six payload words across four separate 64-bit MMIO stores exercises the
 * actual structural fix this pass makes: a recognized header's own
 * declared payload persists across MMIO accesses rather than being
 * misparsed as fresh headers (see the header comment on
 * SI_CMD_HDR_CLASS_STATE, hw/misc/sgi-mgras.c).
 */
static void test_re_blockcopy_execute(void)
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
    /* Payload words 0 and 1, in a separate store: word 0, the source
     * origin, is this device's own evidenced field. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)((10u << 16) | 10u) << 32) | 0);
    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, 0);
    /* Payload words 2 and 3, in yet another store. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, 0);
    /* Payload words 4 and 5 complete the header's own declared payload
     * and dispatch the copy. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(si_test_pixel(qts, 50, 50), ==, 0x00654321);
    g_assert_cmphex(si_test_pixel(qts, 51, 51), ==, 0x00654321);
    /* Outside the destination rectangle stays untouched. */
    g_assert_cmphex(si_test_pixel(qts, 52, 52), ==, 0);

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
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_PACKEDCOLOR, 0x00445566));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (2u << 16) | 2u));
    /* Enable the display before the move: si_post_load has to derive
     * display_enabled from the migrated vc3_reg array itself, since that
     * bool is not migrated directly (see the header comment on
     * si_post_load, hw/misc/sgi-mgras.c). */
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    si_migrate(from, uri);

    /* The completed context recognizer survives migration, and so does the
     * persistent direct-command register file: completing the fill on the
     * destination with state programmed before the move proves it. */
    g_assert_cmphex(qtest_readl(to, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 SI_COMMAND_EXEC(REG_BLOCK_END, (3u << 16) | 3u));
    g_assert_cmphex(si_test_pixel(to, 2, 2), ==, 0x00445566);
    /* The migrated device's display surface reflects both the enable bit
     * and the fill: si_post_load correctly re-derived display_enabled and
     * forced a fresh conversion rather than leaving the surface on
     * whatever si_realize's own pre-load reset last put there. */
    g_assert_cmphex(si_test_display_pixel(to, 2, 2), ==, 0x00665544);

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
    qtest_add_func("sn0/origin2000/mgras/re-point-execute",
                   test_re_point_execute);
    qtest_add_func("sn0/origin2000/mgras/re-line-execute",
                   test_re_line_execute);
    qtest_add_func("sn0/origin2000/mgras/re-char-h-execute",
                   test_re_char_h_execute);
    qtest_add_func("sn0/origin2000/mgras/re-blockcopy-execute",
                   test_re_blockcopy_execute);
    qtest_add_func("sn0/origin2000/mgras/migration", test_migration);

    return g_test_run();
}
