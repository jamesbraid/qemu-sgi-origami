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
#define REG_ENTRY               SI_REG(0x130500)
#define REG_CONTEXT_COMMAND     SI_REG(0x120400)
#define REG_VC3_INDEXDATA       SI_REG(0x072038)
#define REG_VC3_INDEX           SI_REG(0x072008)
#define REG_VC3_DATA            SI_REG(0x0720b0)
#define REG_VC3_RAM             SI_REG(0x072190)
#define REG_BOARD_INVENTORY     SI_REG(0x072488)
#define REG_CMAP_WRITE_INDEX    SI_REG(0x070c30)
#define REG_CMAP_DATA           SI_REG(0x070d18)

#define SI_ENTRY_WORD           0x000004fc
#define SI_FINAL_TOKEN           0x0000f400
#define SI_PENDING_FLAGS         0x00080000
#define SI_PENDING_CONTEXT       0x10000000
#define SI_CONTEXT_WORDS         63

#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE(v)    ((v) & 1)

#define SI_PART             0xc003
#define SI_MANUFACTURER     0x02aa

/* A direct 64-bit command record: payload32 | (selector << 34). */
#define SI_COMMAND(reg, value) \
    ((((uint64_t)(0x60001 + ((reg) << 4))) << 34) | (uint32_t)(value))

/* The VC3's indexed write port: register index in the top byte, 16-bit
 * value in bits 23:8. */
#define VC3_INDEXDATA(index, data16) \
    (((uint32_t)(index) << 24) | (((uint32_t)(data16) & 0xffff) << 8))

#define REG_FILL_CLIP_START     0x147
#define REG_FILL_CLIP_END       0x148
#define REG_FILL_COLOR          0x140
#define REG_BLOCK_START         0x46
#define REG_BLOCK_END           0x47

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
 * internals this pass did not settle with confidence. What this test holds
 * sgi-mgras itself to is its own half of the contract: composing the
 * documented wire form (widget field, vector, address; see the header
 * comment on int_dest_upper and si_deliver_interrupt) and issuing exactly
 * one send per pending&mask transition. x-test-last-interrupt-addr/-value
 * are host-only introspection recording what si_deliver_interrupt last
 * composed, independent of whether the switch actually routed it anywhere.
 */
#define HUB_WIDGET              9
#define HUB_REG_OFFSET          0x90

static void test_interrupt_packet_delivery(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    const unsigned int vector = 50;
    const uint64_t expected_addr =
        ((uint64_t)HUB_WIDGET << 48) | HUB_REG_OFFSET;

    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, 0);

    qtest_writel(qts, REG_INT_DEST_UPPER, (HUB_WIDGET << 16) | (vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_IRQ_MASK, SI_PENDING_FLAGS);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, SI_ENTRY_WORD);

    /* Bit 8 raises, low byte carries the vector (sgi-mgras.c's
     * si_deliver_interrupt, mirroring sgi_bridge_deliver_interrupt). */
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, vector | 0x100);

    /* Acknowledging the pending bit is itself a pending&mask level
     * transition (asserted -> deasserted), so it sends its own packet,
     * this time without the raise bit. */
    qtest_writel(qts, REG_PENDING_ACK, SI_PENDING_FLAGS);
    g_assert_cmphex(si_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, vector);

    qtest_quit(qts);
}

static void test_fifo_full_and_unsupported_access(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    /*
     * Fill the command FIFO without ever completing an operation: every
     * record stores state only, none names the fill's execute register.
     * Stop one short of the 256-record capacity: full occupancy does not
     * fit the status register's eight-bit field and reads back as zero,
     * indistinguishable from empty (docs/reviews/
     * si-functional-approximation-review-20260916.md, blocker 7 -- an
     * open gap this test works around rather than resolves).
     */
    for (int i = 0; i < 255; i++) {
        qtest_writeq(qts, REG_CONTEXT_COMMAND,
                     SI_COMMAND(REG_FILL_CLIP_START, 0));
    }
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_STATUS) & 0xff, ==, 255);

    /* The 256th record still fits. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_FILL_CLIP_START, 0));

    /* The 257th must be refused, and the sticky error latches. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND, SI_COMMAND(REG_FILL_CLIP_START, 0));
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==,
                     0x80000000);

    qtest_quit(qts);
}

static void test_direct_fill_and_reset(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    si_enter_context(qts);

    /* One clipped 4x4 rectangle at (10,10)-(13,13), packed color. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILL_CLIP_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILL_CLIP_END, (13u << 16) | 13u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_FILL_COLOR, 0x00112233));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_START, (10u << 16) | 10u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 SI_COMMAND(REG_BLOCK_END, (13u << 16) | 13u));

    /* FIFO drains once the execute record commits the fill. */
    g_assert_cmphex(qtest_readl(qts, REG_FIFO_STATUS) & 0xff, ==, 0);

    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0x00112233);
    /* Outside the clipped rectangle stays untouched. */
    g_assert_cmphex(si_test_pixel(qts, 20, 11), ==, 0);

    /* Reset returns the device to its documented cleared state. */
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(qtest_readl(qts, REG_PENDING), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ENTRY), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_CONTEXT_COMMAND), ==, 0);
    g_assert_cmphex(si_test_pixel(qts, 11, 11), ==, 0);

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
                 SI_COMMAND(REG_FILL_CLIP_START, (1u << 16) | 1u));

    si_migrate(from, uri);

    /* The completed context recognizer and the one stored, uncommitted
     * FIFO record both have to survive migration. */
    g_assert_cmphex(qtest_readl(to, REG_CONTEXT_COMMAND), ==, SI_FINAL_TOKEN);
    g_assert_cmphex(qtest_readl(to, REG_FIFO_STATUS) & 0xff, ==, 1);

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
    qtest_add_func("sn0/origin2000/mgras/context-recognizer-ordering",
                   test_context_recognizer_ordering);
    qtest_add_func("sn0/origin2000/mgras/entry-requires-prefix",
                   test_entry_requires_prefix);
    qtest_add_func("sn0/origin2000/mgras/context-rejects-wrong-token",
                   test_context_rejects_wrong_final_token);
    qtest_add_func("sn0/origin2000/mgras/pending-mask-and-reassert",
                   test_pending_mask_and_reassert);
    qtest_add_func("sn0/origin2000/mgras/interrupt-packet-delivery",
                   test_interrupt_packet_delivery);
    qtest_add_func("sn0/origin2000/mgras/fifo-full-and-unsupported-access",
                   test_fifo_full_and_unsupported_access);
    qtest_add_func("sn0/origin2000/mgras/direct-fill-and-reset",
                   test_direct_fill_and_reset);
    qtest_add_func("sn0/origin2000/mgras/vc3-sram-round-trip",
                   test_vc3_sram_round_trip);
    qtest_add_func("sn0/origin2000/mgras/vc3-register-file",
                   test_vc3_register_file);
    qtest_add_func("sn0/origin2000/mgras/board-inventory-registers",
                   test_board_inventory_registers);
    qtest_add_func("sn0/origin2000/mgras/cmap-index-addresses-the-right-slot",
                   test_cmap_index_addresses_the_right_slot);
    qtest_add_func("sn0/origin2000/mgras/migration", test_migration);

    return g_test_run();
}
