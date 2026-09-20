/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-mgras-raster.h"

/*
 * The direct-command selector shape and the register roles this bounded 2D
 * interpreter implements. si_raster_command decodes a 64-bit record as
 * payload32 | (selector << 34): selector = 0x60001 | (reg << 6) for a
 * plain state write, or 0x70001 | (reg << 6) for the execute-form write of
 * the same register, reg a 9-bit index into the persistent register file
 * (SGIMgrasState.rss). Corrected from this device's original decoder,
 * which read the register field at the wrong bit position and would have
 * rejected every real desktop record as out of range (docs/designs/
 * 2026-09-17-si-viz-pixel-output-design.md and its evidence trail,
 * docs/plans/si-graphics-widget-wiring-handoff-20260916.md's "Pixel output
 * evidence trail").
 *
 * That handoff quotes the check as (selector & 0x1fffc03f) == 0x60001/
 * 0x70001, but bit 14 of that mask is set where the register field needs
 * it clear: reconstructing selector = 0x60001 | (reg << 6) | (exec ?
 * 0x10000 : 0) for all six selectors the handoff cites from the shipped
 * driver's own disassembly (0x651c1, 0x65201, 0x61181, 0x711c1, 0x653c1,
 * 0x65841) only lands every one of them on its claimed public register
 * name (0x46 BLOCKXYSTARTI, 0x47 BLOCKXYENDI as an execute write, 0x161
 * PP1FILLMODE, and so on -- see OpenBSD's impactreg.h, in refs/) under
 * 0x1fff803f. This device uses the corrected constant; the mismatch is a
 * real gap in the design handoff's own worked example, not a re-derivation
 * of the design.
 *
 * Anything that doesn't match either shape -- MgrasStart's own HQ-local
 * configuration words (0x0000ff04/0x00008c04 in the record's high half)
 * and the HQ-local DMA words the driver also submits at this offset
 * (0x000c0004, 0x000a0204, 0x00080004) -- decodes to neither pattern
 * either, so si_raster_command accepts them silently as opaque traffic instead
 * of needing their own carve-out, the same "accept as opaque state"
 * treatment this device gives every other undocumented register.
 */


#define SI_CMD_SELECTOR_MASK 0x1fff803f
#define SI_CMD_SELECTOR_STATE 0x60001
#define SI_CMD_SELECTOR_EXEC 0x70001

/*
 * A 64-bit write to 0x120400 that isn't a direct-command state/execute
 * record (the shapes above) still carries ordinary command-FIFO traffic:
 * each 32-bit half is its own token header, class<<16 | reg<<8 |
 * payload_bytes, high word first. Root-caused from the DDX's own
 * mgrasCopyPixels/mgrasDrawPixels fence sequence (docs/datasheets/
 * mgras-hq4.md's 0x020010/0x020018 row): it pushes header 0x000e0400
 * (class SI_CMD_FIFO_FENCE_CLASS, reg SI_CMD_FIFO_FENCE_REG, no payload)
 * right after clearing SGI_MGRAS_HQ_FLAGS bit SI_HQ_FLAG_FENCE, then polls
 * that same bit. On real hardware the HQ4 sets it as a side effect of
 * processing this one token; this is the only header either the DDX or the
 * kernel object is evidenced to depend on a side effect from, so it is the
 * only one this interpreter dispatches -- see si_command_fifo_token.
 */
#define SI_CMD_FIFO_FENCE_CLASS 0x000e
#define SI_CMD_FIFO_FENCE_REG 0x04
#define SI_HQ_FLAG_FENCE 0x00010000

/*
 * Two more header classes in the same command-FIFO token-stream grammar as
 * the fence header above: class 0x18 a state write, class 0x1c the
 * matching execute-form trigger for the same register -- a second, generic
 * on-the-wire encoding for the same persistent register file
 * (SGIMgrasState.rss) the direct-command 64-bit record shape above already
 * addresses by register number, not a separate register space. A header
 * token only has 8 free bits next to its own class byte, but reg is 9
 * bits, so reg's own bit 8 is folded into the class byte's bit 0: 0x18 and
 * 0x19 are the same state-write class (reg < 0x100 vs. reg >= 0x100), and
 * 0x1c/0x1d are the same execute class the same way. Confirmed by
 * reconstructing the header for each of this device's own evidenced
 * registers below -- 0x00185f04 for ALPHA (reg 0x05f), 0x00184604 for
 * BLOCKXYSTARTI (reg 0x046), and so on -- each one only lands on its
 * claimed register when the class byte's low bit is folded into the
 * register field rather than kept as part of the class.
 *
 * Disassembling the shipped 2D driver (impactsr.so) and the retained
 * kernel driver's own object together against this project's already-
 * public register names (OpenBSD's impactreg.h) settled which header
 * classes and registers a real desktop session actually uses. The eight
 * highest-traffic state-write registers by that census are ALPHA (0x05f),
 * BLOCKXYSTARTI (0x046, already named below), DRBPOINTERS (0x16d),
 * COLORMASKMSBS (0x162, already named), COLORMASKLSBSA (0x163),
 * PACKEDCOLOR (0x05b, already named), PP1FILLMODE (0x161, already named)
 * and GLINE_XSTARTF (0x00c). High traffic does not mean high relevance to
 * painting: COLORMASKMSBS turned out not to be the register a fill's own
 * plane mask reads from -- see si_draw_fill's own header comment and the
 * reset-defaults comment in si_reset for the live-traced correction. A
 * recognized state-write header always
 * declares one 32-bit payload word in this census (payload_bytes 0x04 in
 * every header above); this device stores that word into rss[reg] the
 * same way the direct-record grammar's own state writes do
 * (si_raster_rss_write),
 * so either wire encoding feeds the same persistent state the execute
 * registers below read. Any other reg under these two classes -- roughly
 * three dozen more by the same census, none of them consumed by an
 * execute implementation yet -- is still stored the same generic way
 * rather than left unhandled, without needing its own named macro.
 *
 * The matching execute class (0x1c/0x1d) is this device's real "go do it"
 * trigger for drawing operations -- see si_raster_command_fifo_dispatch and the
 * si_draw_* functions below. BLOCKXYENDI (0x047), CHAR_H (0x070) and
 * BKGRD_BA (0x141) already trigger through the direct-record grammar
 * above; this device also reaches RE_POINT (0x040), RE_LINE (0x041) and
 * RE_BLOCKCOPY (0x045) through this header class, so a guest that issues
 * either wire encoding for the same register reaches the same drawing
 * code. CHAR_L (0x071) is not a mono-glyph trigger -- see
 * si_draw_glyph_row's own header comment.
 *
 * A header's own declared payload can span the boundary between two
 * separate 64-bit MMIO stores to this offset, or trail a header that
 * itself arrived in the low half of one store, with its payload starting
 * in the high half of the next -- si_command_fifo_token below tracks this
 * with a persistent word counter (SGIMgrasState.cmd_token_*) rather than
 * the old per-call decode, which could not represent "still waiting for a
 * payload word from a previous MMIO access" at all. This tracking is
 * scoped to the two classes above, the only ones this device interprets a
 * payload for: applying it to every header, including ones this device
 * does not recognize, would misparse real traffic this device already
 * handles correctly. The fence sequence's own second push (SI_FENCE_PUSH_2
 * in the qtest) carries an unrecognized word, 0xb1, in its high half,
 * immediately followed by the fence header itself in the low half of the
 * very same store; read as a header, 0xb1's own low byte (also 0xb1)
 * would declare a payload_bytes field that is not even a whole number of
 * 32-bit words, and honoring it would swallow the fence header that
 * follows as bogus payload data instead of dispatching it. This device's
 * own existing "don't guess a payload obligation for traffic it doesn't
 * understand" treatment already avoided that; this change only extends
 * real payload tracking to the classes this device newly interprets, not
 * to unrecognized traffic in general.
 *
 * This tracking only covers 64-bit (8-byte) MMIO stores to this offset --
 * si_raster_command, the only caller -- since that is where every evidenced
 * header/payload token exchange in this device's own census arrives (the
 * public contract's own words, "then 64-bit command words"). A 4-byte
 * store to this same offset takes a completely separate path,
 * si_context_write, whose own protocol-word handling (0xf200/0xf400/
 * 0xfa00-shaped words MgrasStart and MgrasCtxsw submit after the context
 * switch completes) is untouched by this change and keeps working exactly
 * as before -- deliberately: those 4-byte writes are evidenced elsewhere
 * in this same file to carry no real command-FIFO header content (the
 * fence sequence's own first push, header 0x00080b04, declares payload
 * that arrives this way and is accepted as opaque, never actually
 * consumed), so folding them into this parser would only add risk without
 * an evidenced payoff.
 */
#define SI_CMD_HDR_CLASS_STATE 0x0018
#define SI_CMD_HDR_CLASS_EXEC 0x001c

/*
 * Register roles this interpreter actually consumes, named from OpenBSD's
 * impactreg.h and X.Org's impact_regs.h (both public, in refs/). Every
 * other register index is still stored in the persistent file above; only
 * these drive the fill/glyph/PIO primitives below.
 */
#define SI_REG_BLOCKXYSTARTI 0x046
#define SI_REG_BLOCKXYENDI 0x047
#define SI_REG_PACKEDCOLOR 0x05b
#define SI_REG_CHAR_H 0x070
#define SI_REG_CHAR_L 0x071
#define SI_REG_BKGRD_RG 0x140
#define SI_REG_BKGRD_BA 0x141
#define SI_REG_PP1FILLMODE 0x161
#define SI_REG_COLORMASKMSBS 0x162

/*
 * RE execute registers this device newly implements, reached through the
 * command-FIFO token-header grammar above
 * (see si_raster_command_fifo_dispatch);
 * named the same way as the roles above.
 */
#define SI_REG_RE_POINT 0x040
#define SI_REG_RE_LINE 0x041
#define SI_REG_RE_BLOCKCOPY 0x045

/*
 * The same physical register number as RE_BLOCKCOPY above (0x045) under its
 * other public name, IR_ALIAS -- OpenBSD's public driver (impact.c) issues
 * IR_ALIAS as the universal execute trigger nearly every 2D primitive ends
 * with. RE_BLOCKCOPY above names what this register does when reached
 * through the command-FIFO token-header grammar's own execute class (never
 * evidenced live, see si_draw_block_copy's own header comment); IR_ALIAS
 * names what it does reached through the direct-record grammar's own
 * execute-form write instead (selector 0x70001, si_raster_command) -- a
 * different wire path this device did not dispatch at all before this pass,
 * live-
 * traced as the actual trigger for 382 real fills in a full boot-to-desktop
 * session. See si_draw_ir_alias_fill's own header comment for the evidence.
 */
#define SI_REG_IR_ALIAS 0x045

/*
 * State-write registers named for the census above but not yet consumed
 * by any execute implementation -- stored generically in the same
 * persistent register file (rss[]) so a later pass can wire them up
 * without another register-file redesign.
 */
#define SI_REG_ALPHA 0x05f
#define SI_REG_COLORMASKLSBSA 0x163
#define SI_REG_GLINE_XSTARTF 0x00c
#define SI_REG_DRBPOINTERS 0x16d

/*
 * COLORMASKLSBSA's own companion register: OpenBSD's driver
 * (sys/arch/sgi/dev/impact.c, "RSS initialization") sets it to the same
 * 0xffffff value as COLORMASKLSBSA, once, at attach, and nothing in the
 * disassembled evidence this device's own fill/glyph/PIO/point/line
 * functions draw on distinguishes a separate role for it -- named for
 * documentation, not consumed as a second mask here.
 */
#define SI_REG_COLORMASKLSBSB 0x164

static void si_raster_rss_write(SGIMgrasState *s, uint32_t reg,
                                uint32_t value);
static void si_raster_command_execute(SGIMgrasState *s, uint32_t reg);
static void si_raster_command_fifo_dispatch(SGIMgrasState *s);

/*
 * One command-FIFO token header or payload word reaching 0x120400 outside
 * the direct-command state/execute shape -- see this file's header
 * comment on SI_CMD_FIFO_FENCE_CLASS for the fence header and on
 * SI_CMD_HDR_CLASS_STATE for the state-write/execute classes and the
 * persistent payload-word tracking below. Everything else this
 * interpreter doesn't recognize is logged and left alone, the same
 * "accept, don't invent meaning" treatment this device gives other
 * undocumented traffic -- and, as before this device tracked any payload
 * at all, an unrecognized header's own payload_bytes field is reported in
 * the log but not used to skip words, since nothing evidenced says what
 * an unrecognized header's payload actually contains (see the header
 * comment on SI_CMD_HDR_CLASS_STATE for why guessing wrong here would be
 * dangerous, not just imprecise).
 */
static void si_command_fifo_token(SGIMgrasState *s, uint32_t token)
{
    uint32_t cmd_class, reg, payload_bytes, true_class;

    if (s->cmd_token_words_remaining > 0) {
        if (s->cmd_token_payload_count <
            SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
            s->cmd_token_payload[s->cmd_token_payload_count++] = token;
        }
        if (--s->cmd_token_words_remaining == 0) {
            si_raster_command_fifo_dispatch(s);
        }
        return;
    }

    cmd_class = token >> 16;
    reg = (token >> 8) & 0xff;
    payload_bytes = token & 0xff;

    if (cmd_class == SI_CMD_FIFO_FENCE_CLASS && reg == SI_CMD_FIFO_FENCE_REG &&
        payload_bytes == 0) {
        s->hq_flags |= SI_HQ_FLAG_FENCE;
        return;
    }

    true_class = cmd_class & ~1u;
    if (true_class == SI_CMD_HDR_CLASS_STATE ||
        true_class == SI_CMD_HDR_CLASS_EXEC) {
        s->cmd_token_class = true_class;
        s->cmd_token_reg = ((cmd_class & 1) << 8) | reg;
        s->cmd_token_payload_count = 0;
        s->cmd_token_words_remaining = payload_bytes / 4;
        if (s->cmd_token_words_remaining == 0) {
            si_raster_command_fifo_dispatch(s);
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented command-FIFO token, "
                  "class 0x%04x reg 0x%02x payload %u bytes\n", cmd_class,
                  reg, payload_bytes);
}

void si_raster_command(SGIMgrasState *s, uint64_t word)
{
    uint32_t selector = (word >> 34) & 0xffffff;
    uint32_t value = word;
    uint32_t reg;

    if (s->cmd_token_words_remaining == 0) {
        /*
         * Only tried where the command-FIFO token stream is between
         * tokens: the direct-record shape occupies a full 64-bit store, so
         * it cannot start mid-payload for a header parsed from an earlier
         * store (see the header comment on SI_CMD_HDR_CLASS_STATE).
         */
        bool state = (selector & SI_CMD_SELECTOR_MASK) == SI_CMD_SELECTOR_STATE;
        bool exec = (selector & SI_CMD_SELECTOR_MASK) == SI_CMD_SELECTOR_EXEC;

        if (state || exec) {
            reg = (selector >> 6) & 0x1ff;
            si_raster_rss_write(s, reg, value);
            if (exec) {
                si_raster_command_execute(s, reg);
            }
            return;
        }
    }
    /*
     * Not a direct-command state/execute record (or this device is still
     * mid-payload for a command-FIFO header parsed from an earlier store).
     * Either way, ordinary command-FIFO token-stream traffic: two 32-bit
     * tokens, high word first -- see si_command_fifo_token.
     */
    si_command_fifo_token(s, (uint32_t)(word >> 32));
    si_command_fifo_token(s, (uint32_t)word);
}

static uint32_t si_logic_op(uint8_t op, uint32_t dst, uint32_t src)
{
    switch (op & 0xf) {
    case 0x0: return 0;
    case 0x1: return src & dst;
    case 0x2: return src & ~dst;
    case 0x3: return src;
    case 0x4: return ~src & dst;
    case 0x5: return dst;
    case 0x6: return src ^ dst;
    case 0x7: return src | dst;
    case 0x8: return ~(src | dst);
    case 0x9: return ~(src ^ dst);
    case 0xa: return ~dst;
    case 0xb: return src | ~dst;
    case 0xc: return ~src;
    case 0xd: return ~src | dst;
    case 0xe: return ~(src & dst);
    default: return ~0u; /* 0xf: GXset */
    }
}

static uint32_t si_paint_pixel(uint8_t op, uint32_t dst, uint32_t src,
                               uint32_t mask)
{
    uint32_t result = si_logic_op(op, dst, src) & 0x00ffffff;
    return (result & mask) | (dst & ~mask);
}

/*
 * Shared rectangle-fill pixel loop for si_draw_fill and
 * si_draw_ir_alias_fill below: paints the rectangle named by two opposite
 * corners (each x<<16|y) with a given colour, logic op and plane mask.
 * Endpoints are normalized to whichever corner is actually smaller/larger
 * rather than requiring xy0 to be the top-left corner -- BLOCKXYENDI's own
 * real traffic always already has xy0 <= xy1 in both axes, but IR_ALIAS's
 * does not (see si_draw_ir_alias_fill's own header comment for the live
 * trace evidence), so normalizing here instead of rejecting a reversed
 * pair is what lets both trigger registers share this one loop.
 *
 * is_index records, per touched pixel in s->wid[], whether colour is a
 * CMAP index (true) or a direct 0x00BBGGRR value (false) -- see the header
 * comment on si_gfx_update's own PseudoColor-vs-TrueColor paragraph for why
 * this replaced a device-global mode flag. s->wid[] is otherwise-unused
 * per-pixel storage already allocated alongside front[] (si_realize); nothing
 * before this pass gave it a role.
 */
static void si_fill_rect(SGIMgrasState *s, uint32_t xy0, uint32_t xy1,
                         uint32_t color, uint8_t op, uint32_t mask,
                         bool is_index)
{
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    int x_lo = MIN(x0, x1), x_hi = MAX(x0, x1);
    int y_lo = MIN(y0, y1), y_hi = MAX(y0, y1);

    if (x_lo >= SGI_MGRAS_WIDTH || y_lo >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x_hi = MIN(x_hi, SGI_MGRAS_WIDTH - 1);
    y_hi = MIN(y_hi, SGI_MGRAS_HEIGHT - 1);
    for (int y = y_lo; y <= y_hi; y++) {
        for (int x = x_lo; x <= x_hi; x++) {
            unsigned int i = y * SGI_MGRAS_WIDTH + x;
            uint32_t *p = &s->front[i];
            *p = si_paint_pixel(op, *p, color, mask);
            s->wid[i] = is_index;
        }
    }
}

/*
 * Solid fill, triggered by BLOCKXYENDI's own execute-form write: rectangle
 * from BLOCKXYSTARTI/BLOCKXYENDI (x<<16|y each), colour from PACKEDCOLOR,
 * logic op from PP1FILLMODE bits 31:26, plane mask from COLORMASKLSBSA
 * (design doc's ordered plan, step 2, corrected against OpenBSD's driver
 * -- see si_logic_op's own header comment and the reset defaults'
 * comment in si_reset for the evidence). All four come from the
 * persistent register file, not this record -- a driver that only ever
 * set colour and fill mode once still gets them on every later fill.
 * COLORMASKMSBS (0x162) is a distinct, real register -- OpenBSD's own
 * driver programs it too, to 0, once at attach -- but it covers bits
 * 31:24, always zero in this device's 24-bit colour values, so it plays
 * no part in a 24-bit fill's own visible result; COLORMASKLSBSB (0x164)
 * is set alongside COLORMASKLSBSA in the one evidenced sequence and is
 * not separately consumed here, since nothing distinguishes their two
 * roles in that same evidence.
 */
static void si_draw_fill(SGIMgrasState *s)
{
    uint32_t color = s->rss[SI_REG_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;

    si_fill_rect(s, s->rss[SI_REG_BLOCKXYSTARTI], s->rss[SI_REG_BLOCKXYENDI],
                color, op, mask, false);
}

/*
 * Solid fill, triggered by IR_ALIAS's own execute-form write (register
 * 0x045, reached only through the direct-record grammar -- see the header
 * comment on SI_REG_IR_ALIAS). Before this pass, an IR_ALIAS execute had no
 * dispatch case at all in si_raster_command_execute and fell to its logged
 * default, painting nothing; OpenBSD's public driver (impact.c) issues
 * IR_ALIAS as the execute trigger nearly every 2D primitive ends with, so
 * this was a real gap, not a deferred corner case -- confirmed live by
 * decoding a full boot-to-desktop session's own trace
 * (/storage/nvme/origami/runs/irix-20260918T112956-2cabd8/qemu.trace):
 * IR_ALIAS executes 382 times with real, varying block rectangles,
 * including two genuine full/half-screen bands ((0,0)-(1279,1023) and
 * (0,512)-(1279,1023)) that match the desktop's own background paint.
 *
 * Reuses si_fill_rect (shared with si_draw_fill above) for the same
 * rectangle/logic-op/mask sourcing, but colour comes from BKGRD_RG
 * (0x140) rather than PACKEDCOLOR -- a deliberate deviation from this
 * pass's own starting assumption (that IR_ALIAS's colour, like
 * BLOCKXYENDI's, comes from PACKEDCOLOR), made because the same decoded
 * trace directly contradicts it: PACKEDCOLOR reads 0x000000 (or is not
 * yet written) at every one of the 382 real IR_ALIAS executes, while
 * BKGRD_RG carries real, varying values (0xff0ff0, 0x11, 0x7) at the same
 * points -- the opposite of BLOCKXYENDI's own 2,169 real executes in the
 * same session, which always read a real, nonzero-capable PACKEDCOLOR.
 * Sourcing colour from PACKEDCOLOR here would make every evidenced
 * IR_ALIAS fill paint black regardless of this fix, which does not match
 * IR_ALIAS's own real traffic being background-band fills rather than
 * foreground/pen-colour drawing. BKGRD_RG is already this device's
 * established "background colour" register (si_draw_glyph_row uses it the
 * same way), so reusing it here is evidence-driven, not a new guess --
 * but it is still a contract change from what this pass was asked to
 * implement, flagged here and in the datasheet as needing independent
 * review against a second trace, not self-cleared.
 *
 * si_fill_rect's own corner-normalizing behaviour (see its header
 * comment) is also load-bearing here: 148 of the 382 real IR_ALIAS
 * executes in the same trace carry BLOCKXYSTARTI with a larger Y than
 * BLOCKXYENDI (e.g. start row 1023, end row 1020) -- the reverse of
 * BLOCKXYENDI's own convention. Rejecting a reversed pair the way the
 * pre-existing fill path did would silently drop 39% of IR_ALIAS's own
 * real traffic.
 *
 * FILLMODE (0x110) and XFRMODE (0x159) are already stored generically in
 * rss[] (state writes to any register land there regardless of whether an
 * execute implementation consumes it) but are not read here: the cited
 * trace shows no criterion in either register's value that distinguishes
 * a second IR_ALIAS-triggered operation from this one -- every one of the
 * 382 executes paints consistently with fill semantics (a rectangle, a
 * mask, a logic op). This device does not invent a discriminator it has
 * no live evidence for; every IR_ALIAS execute currently dispatches to
 * this one implemented operation rather than a guessed multi-mode
 * dispatch. A future pass with evidence of a second operation should add
 * a real FILLMODE/XFRMODE-gated branch here instead of assuming this one
 * covers every case.
 *
 * BKGRD_RG is always a CMAP index here, not a direct colour -- corrected
 * from this pass's own first cut, which stored it as a raw 0x00BBGGRR
 * value and relied on a device-global "most recently programmed XMAP
 * mode" flag (SGI_MGRAS_XMAP_MAIN_MODE) at scan-out time to decide whether
 * to look it up. Replaying the same cited trace end to end (not just the
 * single IR_ALIAS census above) falsifies that flag as a classifier: the
 * real desktop's background rows arrive with BKGRD_RG=0x11 while
 * SGI_MGRAS_XMAP_MAIN_MODE already holds 0x7a4 -- a value this device
 * previously classified TrueColor -- both at the moment of this exact
 * fill and for the rest of the captured session (no further mode write
 * follows), so sampling the flag at fill time instead of scan time makes
 * no difference. What does settle it: this device's own live CMAP state
 * at that same point has index 0x11 loaded with a real teal-ish colour
 * (0x00ecec00, i.e. R=0 G=236 B=236) that matches the desktop's own
 * background exactly, while the previous raw-value interpretation painted
 * RGB(0,0,0x11) -- near-black dark red, the exact bug this pass fixes.
 * The single global mode flag cannot be the right classifier at all: the
 * same trace shows a large tiled TrueColor-looking fill (BKGRD_RG=
 * 0xff0ff0, a plausible direct magenta) interleaved with these indexed
 * ones under that same unchanged mode word, so no single device-wide flag
 * value, sampled at any time, can be right for both. BKGRD_RG's own
 * established role (background colour, distinct from PACKEDCOLOR's
 * foreground ink -- see this function's own trace census above and
 * si_draw_glyph_row's matching background usage) is the real, evidenced
 * split instead: si_fill_rect's new is_index parameter records this in
 * s->wid[] per pixel, and si_gfx_update resolves it through the CMAP at
 * scan time (preserving real indexed-colour hardware's own palette-
 * cycling behaviour -- confirmed live: CMAP slot 0x11 is itself rewritten
 * several times between this fill and the eventual scan, and only the
 * scan-time value matches the expected desktop colour). CONTRACT CHANGE,
 * needs independent review: this replaces the first cut's device-global
 * mode flag with a per-pixel classification keyed on which register
 * supplied the colour, not on SGI_MGRAS_XMAP_MAIN_MODE at all.
 */
static void si_draw_ir_alias_fill(SGIMgrasState *s)
{
    uint32_t color = s->rss[SI_REG_BKGRD_RG] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;

    si_fill_rect(s, s->rss[SI_REG_BLOCKXYSTARTI], s->rss[SI_REG_BLOCKXYENDI],
                color, op, mask, true);
}

/*
 * 1-bit glyph row expansion, triggered by CHAR_H's own execute-form
 * write: one push paints exactly one row of the glyph, using only bits
 * 31:16 of the 32-bit payload (up to 16 real pixels, MSB-first -- bit 31
 * is the row's own leftmost pixel); bits 15:0 are always zero in real
 * traffic and are not painted. This device tracks which row with its own
 * persistent cursor (SGIMgrasState.glyph_row), reset whenever
 * BLOCKXYSTARTI is rewritten (si_raster_rss_write). Set bits paint PACKEDCOLOR,
 * clear bits paint BKGRD_RG.
 *
 * Corrected twice now from this device's original reading (CHAR_L
 * supplies a row's first 32 pixels, CHAR_H a further 32, either register
 * triggering) -- the file's own previous header comment already flagged
 * that split as unconfirmed. Disassembling the shipped 2D driver's own
 * accelerated glyph-blit routines (mgrasImageGlyphBlt at 0x443bd0,
 * mgrasPolyGlyphBlt at 0x4508f0, impactsr.so) settles the trigger
 * register: both compute CHAR_L's own execute-form FIFO selector early
 * but never issue it in this code path, and every glyph pixel push is an
 * independent CHAR_H execute record. This matches this device's own
 * already-recorded live trace exactly: a real desktop session's 496
 * glyph-path execute hits landed on CHAR_H alone, zero on CHAR_L
 * (docs/datasheets/mgras-hq4.md's `0x120400` row) -- not a trace gap, the
 * real contract for this code path.
 *
 * A first attempt at this fix (before this pass) read CHAR_H's own 32
 * bits as a continuous MSB-first bitstream, wrapping to the next row
 * whenever the running pixel count reached the block rectangle's own
 * width -- a bounded guess this file's own comment already labelled
 * unconfirmed. Booting a real guest with that reading produced scrambled,
 * noise-like pixel garbage in place of text, worse than the blocky-but-
 * shaped output it replaced, so it was wrong. Disassembling the row-push
 * loop itself (mgrasImageGlyphBlt 0x444c80-0x444c98) settles the real
 * shape: it reads one 16-bit halfword from the glyph bitmap array (a
 * pointer that advances by exactly 2 bytes per push, `lhu`/`addiu 2`,
 * never re-reading the same halfword), shifts it into bits 31:16 with
 * `sll 0x10` then clears bits 15:0 with a `dsll32`/`dsrl32` pair, and ORs
 * that with the CHAR_H execute selector before the `sd` push -- so a real
 * push's own low 16 bits are always zero, and the loop's own row counter
 * (`ascent+descent`, decremented once per push, from each character's
 * `xCharInfo` metrics -- 0x444b98-0x444bc4) confirms one push is exactly
 * one row, not a fixed 32-pixel span independent of row boundaries.
 * BLOCKXYSTARTI/BLOCKXYENDI are reissued once per character (their own
 * pushes, 0x444c1c/0x444c58, sit inside the same per-character loop body
 * as the row-push loop, using a fresh glyph-sized rectangle from that
 * character's own metrics), not once per string -- this device's
 * existing "reset the row cursor on BLOCKXYSTARTI" behavior already
 * matches that, since both registers are freshly written for every
 * glyph.
 *
 * Which of BKGRD_RG/BKGRD_BA actually carries a paintable background
 * colour is still this device's own approximation, not evidenced by this
 * pass (see si_draw_pio_pixel_pair, which gives BKGRD_BA a different
 * meaning under its own execute trigger). A glyph wider than 16 pixels is
 * also outside this evidence: the same disassembly found a second,
 * heavier-unrolled code shape later in these same functions that packs
 * two halfwords per push and alternates between CHAR_H's own state form
 * and register 0x071's execute form -- structurally close to this
 * device's own first (wrong) CHAR_H/CHAR_L guess -- but nothing pins down
 * when a real driver takes that path, and the live trace above already
 * shows a real desktop session never reaches it (zero 0x071 executes), so
 * it is left unimplemented rather than guessed at.
 *
 * CHAR_L's own execute form is a separate, unimplemented contract on top
 * of that: the same disassembly also found a genuine CHAR_H/CHAR_L
 * pairing in the shipped driver's antialiased glyph path
 * (mgrasPolyGlyphBltTE8 at 0x462790 and its TE6/TE9, Image/Poly
 * siblings) -- there CHAR_H is a plain state write immediately followed
 * by CHAR_L's own execute trigger (0x462d8c-0x462d9c), each carrying four
 * packed 8-bit antialiasing coverage values extracted from a 16-bit
 * fixed-point accumulator, not bitmap bits. Nothing in this device's own
 * evidence shows a stock desktop session reaching that path either, so it
 * stays unimplemented -- CHAR_L's execute form falls through to
 * si_raster_command_execute's own logged default case rather than being wired
 * to this function.
 */
static void si_draw_glyph_row(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint32_t fg = s->rss[SI_REG_PACKEDCOLOR] & 0x00ffffff;
    uint32_t bg = s->rss[SI_REG_BKGRD_RG] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;
    uint32_t bits = s->rss[SI_REG_CHAR_H];
    int y = y0 + s->glyph_row;
    int width;

    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);
    s->glyph_row++;
    if (y > y1 || y >= SGI_MGRAS_HEIGHT) {
        return;
    }
    width = MIN(x1 - x0 + 1, 16);
    for (int i = 0; i < width; i++) {
        bool set = (bits >> (31 - i)) & 1;
        unsigned int idx = y * SGI_MGRAS_WIDTH + x0 + i;
        uint32_t *p = &s->front[idx];
        *p = si_paint_pixel(op, *p, set ? fg : bg, mask);
        /*
         * fg is PACKEDCOLOR (direct colour, matching si_draw_fill/
         * si_draw_point/si_draw_line); bg is BKGRD_RG, always a CMAP
         * index -- see si_draw_ir_alias_fill's own header comment for the
         * live evidence. s->wid[] records which one painted this pixel so
         * si_gfx_update can resolve it correctly at scan time.
         */
        s->wid[idx] = !set;
    }
}

/*
 * 32-bit PIO pixel-pair upload (icons, image puts), triggered by BKGRD_BA's
 * own execute-form write: paints BKGRD_RG as one pixel and the value just
 * written to BKGRD_BA as the next, at this device's own persistent write
 * cursor (SGIMgrasState.pio_x/pio_y), advancing left to right across the
 * block rectangle and wrapping to the next row. Reset whenever
 * BLOCKXYSTARTI is rewritten, the same as the glyph row cursor.
 *
 * No public register name distinguishes "raw pixel pair" traffic from the
 * background-colour pair glyph rendering gives the same two registers --
 * the design doc's own "genuinely unresolved" list names PIO's exact
 * register grammar as needing a live trace, not a guess. This device
 * reuses BKGRD_RG/BKGRD_BA for both, disambiguated only by whether the
 * write to BKGRD_BA carries the execute bit: a plain state write updates
 * the background colour a later glyph row reads, an execute write paints a
 * pixel pair immediately. XFRSIZE/XFRMODE are accepted into the register
 * file but not consulted here.
 *
 * CONTRACT CHANGE, needs independent review: an earlier version of this
 * function kept its pre-existing direct-colour reading here, reasoning
 * that this path was not evidenced live at all (zero BKGRD_BA executes in
 * the cited boot-to-desktop trace). Live-checking a fresh boot-to-desktop
 * capture after that pass (QMP x-test-pixel/x-test-display-pixel against
 * a real desktop background pixel, run irix-20260918T183100-1383ab)
 * found front[] still holding a small raw value (0x11) that scanned out
 * unresolved -- s->wid[] read false there, meaning neither si_draw_
 * ir_alias_fill nor si_draw_glyph_row painted it, and the same session's
 * BLOCKXYENDI traffic (per this device's own re-checked trace census)
 * reads PACKEDCOLOR as 0 at every execute, not a small nonzero value like
 * 0x11 -- ruling out both other wid[]-false paths as the source. This PIO
 * path -- explicitly for "icons, image puts" per its own established
 * role -- is the only remaining one that can paint a BKGRD_RG-sourced
 * value without tagging it, so this pass now treats both pixels[] the
 * same way si_draw_ir_alias_fill treats BKGRD_RG: always a CMAP index,
 * the same "background colour family, not foreground ink" split already
 * evidenced for BKGRD_RG's other consumers, not a new guess.
 */
static void si_draw_pio_pixel_pair(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint32_t pixels[2] = {
        s->rss[SI_REG_BKGRD_RG] & 0x00ffffff,
        s->rss[SI_REG_BKGRD_BA] & 0x00ffffff,
    };
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;

    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);

    for (int i = 0; i < 2; i++) {
        int x = x0 + s->pio_x;
        int y = y0 + s->pio_y;
        unsigned int idx;

        if (y > y1) {
            return;
        }
        idx = y * SGI_MGRAS_WIDTH + x;
        uint32_t *p = &s->front[idx];
        *p = si_paint_pixel(op, *p, pixels[i], mask);
        s->wid[idx] = 1;
        s->pio_x++;
        if (x0 + s->pio_x > x1) {
            s->pio_x = 0;
            s->pio_y++;
        }
    }
}

/*
 * Single-pixel plot, triggered by RE_POINT's own execute-form header
 * (class 0x1c reg 0x040): paints one pixel at BLOCKXYSTARTI in the
 * current colour/logic-op/mask, the same persistent state the fill/line/
 * glyph primitives above already read. This device's own evidence trail
 * names this trigger's public role as mgrasSetPltParam/nfbPolyPoint,
 * which names a multi-point path; nothing evidenced settles how a point
 * list or count would reach this device (no register in this device's own
 * census carries one), so this is a bounded single-point approximation,
 * not a claim that real hardware never plots more than one point per
 * trigger.
 */
static void si_draw_point(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t color = s->rss[SI_REG_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;
    int x = (xy0 >> 16) & 0xffff, y = xy0 & 0xffff;
    unsigned int idx;
    uint32_t *p;

    if (x >= SGI_MGRAS_WIDTH || y >= SGI_MGRAS_HEIGHT) {
        return;
    }
    idx = y * SGI_MGRAS_WIDTH + x;
    p = &s->front[idx];
    *p = si_paint_pixel(op, *p, color, mask);
            /*
             * PACKEDCOLOR is always a direct colour; see si_fill_rect's own
             * header comment on s->wid[]'s role.
             */
    s->wid[idx] = 0;
}

/*
 * Line/segment draw, triggered by RE_LINE's own execute-form header
 * (class 0x1c reg 0x041): a Bresenham line from BLOCKXYSTARTI to
 * BLOCKXYENDI in the current colour/logic-op/mask, the same registers
 * si_draw_fill already reads. Real ImpactSR hardware almost certainly
 * rasterizes with its own (DDA-style) stepping and endpoint/antialiasing
 * rules; Bresenham is this device's own bounded choice for "a correct,
 * visible line between the two named endpoints," not a claim about the
 * exact pixels real hardware would set.
 */
static void si_draw_line(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    uint32_t color = s->rss[SI_REG_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 < SGI_MGRAS_WIDTH && y0 < SGI_MGRAS_HEIGHT) {
            unsigned int idx = y0 * SGI_MGRAS_WIDTH + x0;
            uint32_t *p = &s->front[idx];
            *p = si_paint_pixel(op, *p, color, mask);
            /*
             * PACKEDCOLOR is always a direct colour; see si_fill_rect's
             * own header comment on s->wid[]'s role.
             */
            s->wid[idx] = 0;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/*
 * Framebuffer block copy, triggered by RE_BLOCKCOPY's own execute-form
 * header (class 0x1c reg 0x045): this device's own evidence trail names
 * it as the trigger for copy/readpixels/writeFBSubBlock-class operations
 * and gives it a six-word payload (header payload_bytes 0x18, "IR_ALIAS"
 * per that trail) instead of the usual zero. The destination rectangle is
 * the same persistent BLOCKXYSTARTI/BLOCKXYENDI state every other
 * primitive here reads; the source origin is this header's own first
 * payload word, packed x<<16|y the same way every other block coordinate
 * on this device already is. What the other five payload words carry --
 * direction, plane selection, read-vs-write mode -- is not settled by
 * this device's own evidence, so si_command_fifo_token still consumes
 * them to keep the token stream in sync but this function does not
 * interpret them: a plain in-buffer rectangle copy (GXcopy, no mask) is
 * this device's own bounded approximation of "the one operation the
 * driver is known to trigger this register for," not a confirmed wire
 * contract -- framebuffer-to-framebuffer copy was previously deferred
 * pending exactly this evidence (design doc, "Framebuffer-to-framebuffer
 * copy... deferred pending a live trace of its exact register grammar").
 * Source and destination share one buffer, so overlapping rectangles copy
 * in whichever row order keeps a row's source data from being overwritten
 * before it is read. s->wid[] moves alongside front[] a row at a time, so
 * a copied region keeps whichever pixels were CMAP indices and which were
 * direct colours -- see si_fill_rect's own header comment on s->wid[]'s
 * role.
 */
static void si_draw_block_copy(SGIMgrasState *s, const uint32_t *payload,
                               uint32_t payload_words)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    int sx0, sy0, w, h;
    bool copy_backward;

    if (payload_words < 1 || x0 > x1 || y0 > y1 ||
        x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);
    sx0 = (payload[0] >> 16) & 0xffff;
    sy0 = payload[0] & 0xffff;
    if (sx0 >= SGI_MGRAS_WIDTH || sy0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    w = MIN(x1 - x0 + 1, SGI_MGRAS_WIDTH - sx0);
    h = MIN(y1 - y0 + 1, SGI_MGRAS_HEIGHT - sy0);
    copy_backward = (y0 > sy0) || (y0 == sy0 && x0 > sx0);

    for (int row = 0; row < h; row++) {
        int r = copy_backward ? (h - 1 - row) : row;
        uint32_t *dst_row = &s->front[(y0 + r) * SGI_MGRAS_WIDTH + x0];
        uint32_t *src_row = &s->front[(sy0 + r) * SGI_MGRAS_WIDTH + sx0];
        uint8_t *dst_wid = &s->wid[(y0 + r) * SGI_MGRAS_WIDTH + x0];
        uint8_t *src_wid = &s->wid[(sy0 + r) * SGI_MGRAS_WIDTH + sx0];

        memmove(dst_row, src_row, (size_t)w * sizeof(uint32_t));
        memmove(dst_wid, src_wid, (size_t)w * sizeof(uint8_t));
    }
}

/*
 * Anything an execute record triggers that this bounded interpreter does
 * not implement -- whatever a live trace eventually surfaces beyond the
 * registers named above. Logged and left alone rather than raised through
 * si_error: an error interrupt on every unimplemented primitive would take
 * a real guest's X server down before anything is visible, defeating this
 * phase's purpose (design doc, "Design highlights not already in the
 * design doc" in the evidence trail).
 */
static void si_raster_command_execute(SGIMgrasState *s, uint32_t reg)
{
    switch (reg) {
    case SI_REG_RE_POINT:
        si_draw_point(s);
        s->dirty = true;
        return;
    case SI_REG_RE_LINE:
        si_draw_line(s);
        s->dirty = true;
        return;
    case SI_REG_BLOCKXYENDI:
        si_draw_fill(s);
        s->dirty = true;
        return;
    case SI_REG_IR_ALIAS:
        si_draw_ir_alias_fill(s);
        s->dirty = true;
        return;
    case SI_REG_CHAR_H:
        si_draw_glyph_row(s);
        s->dirty = true;
        return;
    case SI_REG_BKGRD_BA:
        si_draw_pio_pixel_pair(s);
        s->dirty = true;
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented direct-command "
                      "execute, register 0x%03x\n", reg);
        return;
    }
}

/*
 * Shared state-write path for the persistent direct-command register file
 * (SGIMgrasState.rss), used by both the 64-bit direct-record grammar
 * (si_raster_command) and the command-FIFO token-header grammar
 * (si_raster_command_fifo_dispatch) below -- the two are alternate wire
 * encodings
 * for the same register space (see the header comment on
 * SI_CMD_HDR_CLASS_STATE), so either one has to reset this device's own
 * glyph-row/PIO write cursor on a new block rectangle, not just whichever
 * grammar happened to be implemented first.
 */
static void si_raster_rss_write(SGIMgrasState *s, uint32_t reg, uint32_t value)
{
    s->rss[reg] = value;
    if (reg == SI_REG_BLOCKXYSTARTI) {
        /*
         * A new block rectangle restarts this device's own glyph-row/PIO
         * write cursor; see si_draw_glyph_row and si_draw_pio_pixel_pair.
         */
        s->glyph_row = 0;
        s->pio_x = s->pio_y = 0;
    }
}

/*
 * Dispatches a command-FIFO token header once si_command_fifo_token below
 * has collected all of its declared payload (immediately, for the common
 * zero-payload case). A state write stores its one payload word into the
 * persistent register file the same way the direct-record grammar's own
 * state writes do; an execute trigger reuses si_raster_command_execute for
 * every
 * register that grammar already implements, and handles RE_BLOCKCOPY
 * (0x045) separately since it is the one execute register this device's
 * own evidence gives its own payload (see si_draw_block_copy).
 */
static void si_raster_command_fifo_dispatch(SGIMgrasState *s)
{
    uint32_t reg = s->cmd_token_reg;

    if (s->cmd_token_class == SI_CMD_HDR_CLASS_STATE) {
        if (s->cmd_token_payload_count >= 1) {
            si_raster_rss_write(s, reg, s->cmd_token_payload[0]);
        } else {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: state-write command-FIFO "
                          "header, register 0x%03x with no payload word\n",
                          reg);
        }
        return;
    }
    /* SI_CMD_HDR_CLASS_EXEC */
    if (reg == SI_REG_RE_BLOCKCOPY) {
        si_draw_block_copy(s, s->cmd_token_payload, s->cmd_token_payload_count);
        s->dirty = true;
        return;
    }
    si_raster_command_execute(s, reg);
}
