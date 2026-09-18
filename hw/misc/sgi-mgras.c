/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded functional SI Viz endpoint for the Origin 2000 graphics path. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/sgi-mgras.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "trace.h"

#define SI_PENDING_FLAGS 0x00080000
#define SI_PENDING_CONTEXT 0x10000000
#define SI_PENDING_RETRACE 0x00000001
#define SI_PENDING_ERROR 0x00000002
/*
 * A fifth pending source alongside the four above: the HQ4 command FIFO's
 * status/low-water role. The kernel driver's own MgrasFlushHQ3 (called from
 * MgrasCtxsw and MgrasPcxSwap) and mgras_hq4_hiwater_intr's timeout fallback
 * both arm a 15-second watchdog (mgras_fifotimer) whenever they need to wait
 * for this FIFO to drain, and both write bit SI_HQ_ENABLE_LOWATER into
 * SGI_MGRAS_HQ_ENABLE right beforehand -- live-traced from each routine's
 * own disassembly. The only place that clears that bit back out is
 * mgras_hq4_lowater_intr, dispatched by this same pending source once
 * delivered; nothing before this device modelled it, so the watchdog was
 * never disarmed and IRIX panicked roughly 15 seconds after any code path
 * (gfxinit, the X server) opened this board. This device tracks no real FIFO
 * occupancy to know when a genuine low-water transition would happen, so it
 * satisfies the wait immediately on the arming write rather than never --
 * the same "immediate readiness, no modelled delay" approximation already
 * used elsewhere in this file (see si_update_irq and the SGI_MGRAS_HQ_ENABLE
 * write case).
 *
 * Modelling the pending bit alone was not sufficient: mgras_hq4_lowater_intr
 * never actually ran, because every interrupt role this device delivered --
 * this one included -- used the same shared destination and vector (the
 * generic widget destination pair, see the header comment on int_dest_upper),
 * which is wired to MgrasHQ4ErrInterrupt, a printf-only handler that clears
 * nothing. Confirmed live: "HQ4 widget err - 0x0," appears exactly once in
 * every retained panicking console log, right on schedule. Disassembling
 * mgras_attach's own four interrupt-handler registrations against the
 * retained disk's rebuilt kernel settled the real per-role vector table at
 * SGI_MGRAS_HQ_VECTORS (see that header comment); si_update_irq below now
 * delivers this role through its own vector there instead.
 *
 * A third gap surfaced once gfxinit's GE microcode download started
 * succeeding and driver code reached MgrasCtxsw for real (docs/plans/
 * si-graphics-widget-wiring-handoff-20260916.md, "gfxinit clears GE
 * download live, then MgrasCtxsw timeout"): this bit's own clear, via
 * SGI_MGRAS_HQ_ENABLE_CLEAR (0x011058, mgras_hq4_lowater_intr's own
 * disarm write), left SI_PENDING_LOWATER stuck set -- only a full device
 * reset ever cleared it. The first arm/deliver/disarm cycle worked, but a
 * second arming (MgrasFlushHQ3 arms this bit on every ordinary FIFO wait,
 * not just a timeout) found the pending bit already set from the first
 * delivery, so si_update_irq's transition detector never saw a fresh
 * edge and never sent a second packet -- the driver's 15-second FIFO
 * watchdog then expired unopposed, alongside the already-armed
 * context-switch watchdog. The 0x011058 write case now clears this
 * pending bit on the same transition it already detects for
 * SI_HQ_ENABLE_LOWATER, making the role re-triggerable.
 */
#define SI_PENDING_LOWATER 0x00000004
#define SI_HQ_ENABLE_LOWATER 0x10

/*
 * The direct-command selector shape and the register roles this bounded 2D
 * interpreter implements. si_command decodes a 64-bit record as
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
 * either, so si_command accepts them silently as opaque traffic instead
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
 * same way the direct-record grammar's own state writes do (si_rss_write),
 * so either wire encoding feeds the same persistent state the execute
 * registers below read. Any other reg under these two classes -- roughly
 * three dozen more by the same census, none of them consumed by an
 * execute implementation yet -- is still stored the same generic way
 * rather than left unhandled, without needing its own named macro.
 *
 * The matching execute class (0x1c/0x1d) is this device's real "go do it"
 * trigger for drawing operations -- see si_command_fifo_dispatch and the
 * si_draw_* functions below. BLOCKXYENDI (0x047), CHAR_L (0x071) and
 * BKGRD_BA (0x141) already trigger through the direct-record grammar
 * above; this device also reaches RE_POINT (0x040), RE_LINE (0x041),
 * RE_BLOCKCOPY (0x045) and CHAR_H (0x070, si_command_execute's own case)
 * through this header class, so a guest that issues either wire encoding
 * for the same register reaches the same drawing code.
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
 * si_command, the only caller -- since that is where every evidenced
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
 * command-FIFO token-header grammar above (see si_command_fifo_dispatch);
 * named the same way as the roles above.
 */
#define SI_REG_RE_POINT 0x040
#define SI_REG_RE_LINE 0x041
#define SI_REG_RE_BLOCKCOPY 0x045

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

/* Where the Hub's interrupt-modify register sits; data bit 8 selects raise. */
#define SI_HUB_INT_MODIFY_SET 0x100
#define SI_INT_DEST_WIDGET_SHIFT 16
#define SI_INT_DEST_WIDGET_BITS 4
#define SI_INT_DEST_VECTOR_SHIFT 24

/*
 * The HQ4 vector-dest pair's own target-widget field sits at a different
 * shift than the generic pair above (bits 27:24, not 19:16); the address
 * and widget-bit-count shapes are otherwise identical. See the header
 * comment on SGI_MGRAS_HQ_VECTOR_DEST_UPPER.
 */
#define SI_HQ_VECTOR_WIDGET_SHIFT 24
#define SI_HQ_VECTOR_WIDGET_BITS 4

/* Byte offsets into SGI_MGRAS_HQ_VECTORS; see the header comment. */
#define SI_HQ_VECTOR_LOWATER_SHIFT 0
#define SI_HQ_VECTOR_GENERAL_SHIFT 8
#define SI_HQ_VECTOR_RETRACE_SHIFT 16

/*
 * A write to the target's interrupt-modify register; data bit 8 selects
 * raise. Mirrors hw/pci-host/sgi-bridge.c's sgi_bridge_deliver_interrupt.
 */
static void si_send_packet(SGIMgrasState *s, hwaddr where, uint8_t vector,
                           bool set)
{
    uint64_t value = vector | (set ? SI_HUB_INT_MODIFY_SET : 0);

    /* Host-only qtest introspection; see the header comment on the fields. */
    s->test_last_interrupt_addr = where;
    s->test_last_interrupt_value = value;
    address_space_stq(&s->fabric_as, where, value, MEMTXATTRS_UNSPECIFIED,
                      NULL);
}

/*
 * Keep the packet header's widget separate from its complete address,
 * mirroring hw/pci-host/sgi-bridge.c's sgi_bridge_compose_xtalk.
 */
static bool si_compose_xtalk(SGIMgrasState *s, unsigned int widget,
                             uint64_t address, hwaddr *fabric)
{
    if (s->fabric_widget_shift) {
        if (widget >= (1u << SI_INT_DEST_WIDGET_BITS) ||
            address >> s->fabric_widget_shift) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: packet fabric cannot "
                          "encode widget %u address 0x%" PRIx64 "\n", widget,
                          address);
            return false;
        }
        *fabric = ((uint64_t)widget << s->fabric_widget_shift) | address;
    } else {
        *fabric = s->fabric_base + address;
    }
    return true;
}

/*
 * The widget-error role (MgrasHQ4ErrInterrupt): unchanged from this
 * device's original, single delivery path -- the generic widget
 * destination pair, vector in its own top byte. See the header comment on
 * int_dest_upper and SGI_MGRAS_HQ_VECTOR_DEST_UPPER.
 */
static void si_deliver_widget_error(SGIMgrasState *s, bool set)
{
    unsigned int widget = (s->int_dest_upper >> SI_INT_DEST_WIDGET_SHIFT) &
                          ((1u << SI_INT_DEST_WIDGET_BITS) - 1);
    uint64_t addr = ((uint64_t)(s->int_dest_upper & 0xffff) << 32) |
                    s->int_dest_lower;
    uint8_t vector = s->int_dest_upper >> SI_INT_DEST_VECTOR_SHIFT;
    hwaddr where;

    if (!s->int_dest_upper && !s->int_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: widget-error interrupt %s "
                      "with no destination programmed; no packet sent\n",
                      set ? "raised" : "lowered");
        return;
    }
    if (!si_compose_xtalk(s, widget, addr, &where)) {
        return;
    }
    si_send_packet(s, where, vector, set);
}

/*
 * The three HQ4-specific roles (low-water, general, retrace): share the
 * HQ vector-dest pair for target/address, each with its own vector byte
 * from SGI_MGRAS_HQ_VECTORS. See that header comment.
 */
static void si_deliver_hq_role(SGIMgrasState *s, bool set, uint8_t vector,
                               const char *role)
{
    unsigned int widget = (s->hq_vector_dest_upper >>
                           SI_HQ_VECTOR_WIDGET_SHIFT) &
                          ((1u << SI_HQ_VECTOR_WIDGET_BITS) - 1);
    uint64_t addr = ((uint64_t)(s->hq_vector_dest_upper & 0xffff) << 32) |
                    s->hq_vector_dest_lower;
    hwaddr where;

    if (!s->hq_vector_dest_upper && !s->hq_vector_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: %s interrupt %s with no "
                      "HQ vector destination programmed; no packet sent\n",
                      role, set ? "raised" : "lowered");
        return;
    }
    if (!si_compose_xtalk(s, widget, addr, &where)) {
        return;
    }
    si_send_packet(s, where, vector, set);
}

static void si_update_irq(SGIMgrasState *s)
{
    /*
     * Four independent roles, each with its own transition detector and
     * its own packet -- see the header comment on SGI_MGRAS_HQ_VECTOR_DEST_
     * UPPER/LOWER/VECTORS. SI_PENDING_LOWATER bypasses the shared top-level
     * mask: this device has no evidence for which bit of the guest's own
     * irq_mask write (0x011040) real hardware assigns to the status/
     * low-water role (see the header comment on SI_PENDING_LOWATER), while
     * the second-level enable bit that gates it (SGI_MGRAS_HQ_ENABLE's bit
     * 0x10) is live-traced from two independent driver routines. Gating
     * delivery on that bit instead, rather than on a guessed top-level mask
     * bit, is sufficient to satisfy the driver's own wait. The other three
     * roles are ordinary pending&mask sources.
     */
    bool lowater = !!(s->pending & SI_PENDING_LOWATER);
    bool general = !!(s->pending & s->irq_mask &
                      (SI_PENDING_FLAGS | SI_PENDING_CONTEXT));
    bool retrace = !!(s->pending & s->irq_mask & SI_PENDING_RETRACE);
    bool error = !!(s->pending & s->irq_mask & SI_PENDING_ERROR);

    uint8_t lowater_vector = s->hq_vectors >> SI_HQ_VECTOR_LOWATER_SHIFT;
    uint8_t general_vector = s->hq_vectors >> SI_HQ_VECTOR_GENERAL_SHIFT;
    uint8_t retrace_vector = s->hq_vectors >> SI_HQ_VECTOR_RETRACE_SHIFT;

    /* The chip sends on a transition, not while a level is held. */
    if (lowater != s->lowater_asserted) {
        s->lowater_asserted = lowater;
        si_deliver_hq_role(s, lowater, lowater_vector, "low-water");
    }
    if (general != s->general_asserted) {
        s->general_asserted = general;
        si_deliver_hq_role(s, general, general_vector, "general");
    }
    if (retrace != s->retrace_asserted) {
        s->retrace_asserted = retrace;
        si_deliver_hq_role(s, retrace, retrace_vector, "retrace");
    }
    if (error != s->error_asserted) {
        s->error_asserted = error;
        si_deliver_widget_error(s, error);
    }
}

static void si_error(SGIMgrasState *s, hwaddr addr, unsigned size,
                     const char *why)
{
    s->error = 1;
    s->error_addr = addr;
    s->error_width = size;
    s->protocol = 0xff;
    s->pending |= SI_PENDING_ERROR;
    si_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: %s at 0x%" HWADDR_PRIx
                  " width %u\n", why, addr, size);
}

static void si_retrace(void *opaque)
{
    SGIMgrasState *s = opaque;
    if (s->display_enabled) {
        s->retraces++;
        s->pending |= SI_PENDING_RETRACE;
        si_update_irq(s);
    }
    timer_mod(s->retrace_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void si_complete_context(SGIMgrasState *s)
{
    s->ready = true;
    s->protocol = 3;
    s->pending |= SI_PENDING_CONTEXT;
    si_update_irq(s);
}

static void si_context_write(SGIMgrasState *s, uint32_t value)
{
    if (!s->entry_selected || s->protocol == 0xff) {
        si_error(s, 0x120400, 4, "context write in invalid state");
        return;
    }
    if (s->protocol == 1) {
        if (!s->first_ack) {
            si_error(s, 0x120400, 4, "context write before first acknowledgement");
            return;
        }
        if (s->context_words == 63) {
            if (!s->second_ack || value != 0x0000f400) {
                si_error(s, 0x120400, 4, "context token missing");
                return;
            }
            si_complete_context(s);
            return;
        }
        s->context[s->context_words++] = value;
        if (s->context_words == 63) {
            s->pending |= SI_PENDING_CONTEXT;
            si_update_irq(s);
        }
        return;
    }
    /*
     * protocol == 3: the context switch already completed. MgrasStart and
     * MgrasCtxsw submit further 4-byte FIFO words here afterward (0xf200,
     * 0x80000008, 0x40000, 0x111, 0x3700, zero, and several computed
     * values), live-traced with nothing establishing any of them ties to
     * further protocol state. Accepting them as opaque post-completion
     * traffic, rather than flagging every one as a protocol violation, is
     * this device's usual "accept, don't invent meaning" approximation --
     * the check above already excludes every state where a stray write
     * here would be a genuine error (no entry selected, or the sticky
     * error already latched).
     */
}

/*
 * The plane-masked result of applying an X11-style GX logic op (0-15,
 * PP1FILLMODE bits 31:26) between a new colour and the pixel already
 * there. The sixteen values are the standard two-operand boolean
 * functions of (src, dst) X11 GCs already use for raster ops.
 *
 * An earlier pass read this field at bits 31:28 (the top nibble), a
 * bounded guess made explicitly instead of the wider six-bit field
 * OpenBSD's own IMPACTSR_PP1FILLMODE(mode, op) macro reserves for it
 * (sys/arch/sgi/dev/impactreg.h: `(mode) | ((op) << 26)`). A live trace
 * of a real desktop session found that guess wrong: every observed
 * PP1FILLMODE value had a zero top nibble, which decodes to GXclear
 * under the old shift regardless of what the driver actually asked for
 * -- exactly the all-black result that session's own screendump showed.
 * Under the corrected `>> 26` shift the same trace decodes to a mix of
 * GXcopy (3) and GXxor (6), the values a real desktop's fill and line
 * operations plausibly use. `mode` (the macro's other operand, bits
 * 25:0) is this device's own opaque state elsewhere in `rss[]`; nothing
 * here reads it.
 */
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
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    uint32_t color = s->rss[SI_REG_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;

    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            uint32_t *p = &s->front[y * SGI_MGRAS_WIDTH + x];
            *p = si_paint_pixel(op, *p, color, mask);
        }
    }
}

/*
 * 1-bit glyph row expansion, triggered by CHAR_L's own execute-form write:
 * CHAR_L (0x71) supplies the row's first 32 pixels, MSB-first (bit 31 is
 * the glyph's own leftmost pixel); CHAR_H (0x70) supplies a further 32 if
 * the glyph is wider, the common case (an 8- or 16-pixel-wide console
 * font row) needing only CHAR_L. Set bits paint PACKEDCOLOR, clear bits
 * paint BKGRD_RG (design doc's ordered plan: "same fill-mode/colour
 * registers plus a background colour register pair"). One row paints per
 * execute record; this device tracks which row with its own persistent
 * cursor (SGIMgrasState.glyph_row), reset whenever BLOCKXYSTARTI is
 * rewritten (si_command).
 *
 * Neither this CHAR_H/CHAR_L split nor which register of the BKGRD_RG/
 * BKGRD_BA pair actually carries a paintable background colour is settled
 * by this pass's evidence -- the design doc's own "genuinely unresolved"
 * list names glyph chunking as needing a live trace, not a guess. This is
 * this device's own bounded approximation: BKGRD_BA is left as opaque
 * persistent state here (see si_draw_pio_pixel_pair, which gives it a
 * different meaning under its own execute trigger).
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
    uint32_t char_l = s->rss[SI_REG_CHAR_L];
    uint32_t char_h = s->rss[SI_REG_CHAR_H];
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
    width = MIN(x1 - x0 + 1, 64);
    for (int i = 0; i < width; i++) {
        uint32_t half = (i < 32) ? char_l : char_h;
        bool set = (half >> (31 - (i % 32))) & 1;
        uint32_t *p = &s->front[y * SGI_MGRAS_WIDTH + x0 + i];
        *p = si_paint_pixel(op, *p, set ? fg : bg, mask);
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

        if (y > y1) {
            return;
        }
        uint32_t *p = &s->front[y * SGI_MGRAS_WIDTH + x];
        *p = si_paint_pixel(op, *p, pixels[i], mask);
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
    uint32_t *p;

    if (x >= SGI_MGRAS_WIDTH || y >= SGI_MGRAS_HEIGHT) {
        return;
    }
    p = &s->front[y * SGI_MGRAS_WIDTH + x];
    *p = si_paint_pixel(op, *p, color, mask);
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
            uint32_t *p = &s->front[y0 * SGI_MGRAS_WIDTH + x0];
            *p = si_paint_pixel(op, *p, color, mask);
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
 * before it is read.
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

        memmove(dst_row, src_row, (size_t)w * sizeof(uint32_t));
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
static void si_command_execute(SGIMgrasState *s, uint32_t reg)
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
    case SI_REG_CHAR_H:
    case SI_REG_CHAR_L:
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
 * (si_command) and the command-FIFO token-header grammar
 * (si_command_fifo_dispatch) below -- the two are alternate wire encodings
 * for the same register space (see the header comment on
 * SI_CMD_HDR_CLASS_STATE), so either one has to reset this device's own
 * glyph-row/PIO write cursor on a new block rectangle, not just whichever
 * grammar happened to be implemented first.
 */
static void si_rss_write(SGIMgrasState *s, uint32_t reg, uint32_t value)
{
    s->rss[reg] = value;
    if (reg == SI_REG_BLOCKXYSTARTI) {
        /* A new block rectangle restarts this device's own glyph-row/PIO
         * write cursor; see si_draw_glyph_row and si_draw_pio_pixel_pair. */
        s->glyph_row = 0;
        s->pio_x = s->pio_y = 0;
    }
}

/*
 * Dispatches a command-FIFO token header once si_command_fifo_token below
 * has collected all of its declared payload (immediately, for the common
 * zero-payload case). A state write stores its one payload word into the
 * persistent register file the same way the direct-record grammar's own
 * state writes do; an execute trigger reuses si_command_execute for every
 * register that grammar already implements, and handles RE_BLOCKCOPY
 * (0x045) separately since it is the one execute register this device's
 * own evidence gives its own payload (see si_draw_block_copy).
 */
static void si_command_fifo_dispatch(SGIMgrasState *s)
{
    uint32_t reg = s->cmd_token_reg;

    if (s->cmd_token_class == SI_CMD_HDR_CLASS_STATE) {
        if (s->cmd_token_payload_count >= 1) {
            si_rss_write(s, reg, s->cmd_token_payload[0]);
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
    si_command_execute(s, reg);
}

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
        if (s->cmd_token_payload_count < SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
            s->cmd_token_payload[s->cmd_token_payload_count++] = token;
        }
        if (--s->cmd_token_words_remaining == 0) {
            si_command_fifo_dispatch(s);
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
    if (true_class == SI_CMD_HDR_CLASS_STATE || true_class == SI_CMD_HDR_CLASS_EXEC) {
        s->cmd_token_class = true_class;
        s->cmd_token_reg = ((cmd_class & 1) << 8) | reg;
        s->cmd_token_payload_count = 0;
        s->cmd_token_words_remaining = payload_bytes / 4;
        if (s->cmd_token_words_remaining == 0) {
            si_command_fifo_dispatch(s);
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented command-FIFO token, "
                  "class 0x%04x reg 0x%02x payload %u bytes\n", cmd_class,
                  reg, payload_bytes);
}

static void si_command(SGIMgrasState *s, uint64_t word)
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
            si_rss_write(s, reg, value);
            if (exec) {
                si_command_execute(s, reg);
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

/*
 * One GE11 engine slot's control port. A value of 0x200000 or 0x200001
 * (bit 0 selects the verify pass's read-alignment) starts a new load: both
 * the write pointer the load loop fills and the read pointer the verify
 * loop drains reset to zero, and data-port writes append microcode words
 * until the next control write. Any other control word ends the load --
 * the driver's own 0x40038 mode-select word among them, always paired with
 * a 0xd1 data write that is a register value, not a microcode word -- and,
 * if bit 31 is set, rewinds the read pointer, matching the driver's own
 * "control 0x80000000 | count" step right before it starts reading back a
 * pass again.
 */
static void si_ge_control_write(SGIMgrasState *s, unsigned int slot,
                                uint32_t value)
{
    s->ge_active_slot = slot;
    if ((value & ~1u) == 0x200000) {
        s->ge_write_ptr[slot] = 0;
        s->ge_read_ptr[slot] = 0;
        s->ge_read_odd[slot] = value & 1;
        s->ge_load_mode[slot] = true;
    } else {
        if (value & 0x80000000) {
            s->ge_read_ptr[slot] = 0;
        }
        s->ge_load_mode[slot] = false;
    }
}

/*
 * One GE11 engine slot's data port: while a load is active (the most
 * recent control write to this slot was 0x200000/0x200001), each write
 * appends one microcode word to that slot's own buffer, matching the
 * driver's three-writes-per-triple load loop (traced in
 * MgrasDownloadGE11Ucode). Once the driver ends the load with a different
 * control word, further data-port writes are opaque register-select/value
 * traffic -- the 0xd1 word the driver writes right after 0x40038 is the
 * confirmed case -- accepted without touching the microcode buffer or its
 * write pointer, the same "accept as opaque state" treatment this device
 * gives undocumented registers elsewhere.
 */
static void si_ge_data_write(SGIMgrasState *s, hwaddr addr, unsigned size,
                             unsigned int slot, uint32_t value)
{
    uint32_t ptr;

    if (!s->ge_load_mode[slot]) {
        return;
    }
    ptr = s->ge_write_ptr[slot];
    if (ptr >= SGI_MGRAS_GE_WORDS) {
        si_error(s, addr, size, "GE upload overflow");
        return;
    }
    s->ge[slot][ptr] = value;
    s->ge_write_ptr[slot] = ptr + 1;
    s->ge_words[slot] = MAX(s->ge_words[slot], ptr + 1);
}

static uint64_t si_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIMgrasState *s = opaque;
    trace_sgi_mgras_mmio(addr, 0, size, false);
    if (size != 4 && size != 8) {
        si_error(s, addr, size, "unsupported read width");
        return 0;
    }
    switch (addr) {
    case 0x000004:
        return (s->revision << 28) | (s->part << 12) |
               (s->manufacturer << 1) | 1;
    case SGI_MGRAS_INT_DEST_UPPER: return s->int_dest_upper;
    case SGI_MGRAS_INT_DEST_LOWER: return s->int_dest_lower;
    /* The same destination pair, reachable at a second offset too. */
    case SGI_MGRAS_INT_DEST_UPPER_ALT: return s->int_dest_upper;
    case SGI_MGRAS_INT_DEST_LOWER_ALT: return s->int_dest_lower;
    case SGI_MGRAS_WIDGET_CONTROL: return s->widget_control;
    /*
     * MgrasDownloadGE11Ucode reads this back once during the GE verify
     * sequence; the write case below (0x011000) is the same offset's
     * entry-selection-gating prefix state, so this just echoes it back.
     */
    case 0x011000: return s->prefix;
    case SGI_MGRAS_RESET_ARG: return s->reset_arg;
    /* See the header comment: a fixed population code, not an echo. */
    case SGI_MGRAS_RESET_CONTROL: return 0x0f000000;
    /* See the header comment: this SI board class has one GE, no RB. */
    case SGI_MGRAS_BOARD_INVENTORY: return 0x21000000;
    /*
     * MgrasGatherInfo also reads these four offsets once each, for board
     * revision bytes this device's own register table was never evidenced
     * for. Reporting them explicitly, rather than through the sticky
     * unsupported-access path every other undecoded offset takes, matches
     * this device's established "immediate readiness, no invented meaning"
     * approximation: an unknown revision reads as zero instead of raising
     * an error over an access that does not block the boot either way.
     * GatherInfo reads a fifth offset here too, 0x071d00, but that address
     * already has an established meaning (SGI_MGRAS_REPP_STATUS_3, below)
     * whose existing echo-back behaviour already answers GatherInfo's read
     * with whatever the REPP handshake wrote earlier in the same boot,
     * which is what it needs -- nothing to add for it here.
     */
    case 0x071308:
    case 0x071708:
    case 0x028000:
    case 0x028004:
        return 0;
    case SGI_MGRAS_BRINGUP_ARG_2: return s->bringup_arg_2;
    case SGI_MGRAS_BRINGUP_BLOCK_1: return s->bringup_block[0];
    case SGI_MGRAS_BRINGUP_BLOCK_2: return s->bringup_block[1];
    case SGI_MGRAS_BRINGUP_BLOCK_3: return s->bringup_block[2];
    case SGI_MGRAS_SCRATCH_COMMAND: return s->scratch_command;
    case SGI_MGRAS_BRINGUP_ARG_3: return s->bringup_arg_3;
    case SGI_MGRAS_VC3_ARG: return s->vc3_arg;
    case SGI_MGRAS_RE_ARG: return s->re_arg;
    case SGI_MGRAS_I2C_SCRATCH: return s->i2c_scratch;
    case SGI_MGRAS_REPP_STATUS_1: return s->repp_status[0];
    case SGI_MGRAS_REPP_STATUS_3: return s->repp_status[1];
    case SGI_MGRAS_REPP_STATUS_4: return s->repp_status[2];
    case SGI_MGRAS_SYNC_STATUS: return s->sync_status;
    case SGI_MGRAS_BRINGUP_FLAG: return s->bringup_flag;
    case SGI_MGRAS_DAC_LUT_INDEX: return s->dac_lut_index;
    case SGI_MGRAS_DAC_LUT_DATA_1: return s->dac_lut_data[0];
    case SGI_MGRAS_DAC_LUT_DATA_2: return s->dac_lut_data[1];
    case SGI_MGRAS_DAC_CONFIG: return s->dac_config;
    case SGI_MGRAS_DAC_LUT_DATA_1_LOW: return s->dac_lut_data_1_low;
    case 0x008000: return 0;
    /*
     * MgrasCtxsw writes 0x80 to this offset outside the pending/mask/
     * acknowledge protocol the rest of this register already implements,
     * then reads it back masking in bit 0x20 -- neither bit's meaning was
     * captured, so the extra bits are stored separately and reported
     * alongside whatever is genuinely pending, rather than corrupting the
     * real pending state SI_PENDING_FLAGS/CONTEXT/RETRACE/ERROR track.
     */
    case 0x011020: return s->pending | s->hq_status_extra;
    case 0x011040: return s->irq_mask;
    case SGI_MGRAS_HQ_CONFIG_A: return s->hq_config_a;
    case SGI_MGRAS_HQ_CONFIG_B: return s->hq_config_b;
    case SGI_MGRAS_HQ_ENABLE: return s->hq_enable;
    case SGI_MGRAS_HQ_VECTOR_DEST_UPPER: return s->hq_vector_dest_upper;
    case SGI_MGRAS_HQ_VECTOR_DEST_LOWER: return s->hq_vector_dest_lower;
    case SGI_MGRAS_HQ_VECTORS: return s->hq_vectors;
    case SGI_MGRAS_SCHED_ARG_1: return s->sched_arg_1;
    case SGI_MGRAS_SCHED_ARG_2: return s->sched_arg_2;
    case SGI_MGRAS_SCHED_ARG_3: return s->sched_arg_3;
    case SGI_MGRAS_SCHED_ARG_4: return s->sched_arg_4;
    /* Both offsets read the same underlying flag word; see the header
     * comment on SGI_MGRAS_HQ_FLAGS. */
    case SGI_MGRAS_HQ_FLAGS: return s->hq_flags;
    case SGI_MGRAS_HQ_FLAGS_CLEAR: return s->hq_flags;
    /* See the header comment: no evidence for real content, report zero. */
    case SGI_MGRAS_RETRACE_STATUS_1: return 0;
    case SGI_MGRAS_RETRACE_STATUS_2: return 0;
    /*
     * A generic widget bring-up step, not an SI-specific register: the
     * retained PROM's local-IO discovery writes a step-table argument word
     * here for every widget it finds (520<<10 | 65<<2, unrelated to SI's own
     * protocol) and polls this same offset for bit 0x2 before it will
     * continue. hw/pci-host/sgi-bridge.c has no register here either --
     * Bridge's own catch-all invalid-address read returns all-ones, which
     * happens to carry that bit, so Bridge passes this step by accident
     * rather than by implementing it. Reporting it explicitly, rather than
     * through the sticky unsupported-access path every other undecoded
     * offset takes, is the approximation: immediate readiness, no modelled
     * bring-up delay.
     */
    case 0x011098: return 0x2;
    /*
     * This device's first commit returned a plain nonzero "healthy" value
     * here (1) with no evidence behind the exact bits, since nothing had
     * ever exercised the consumer. Live-tracing a boot that finally reaches
     * this poll (only possible once the REPP sync handshake above stopped
     * blocking it) shows the same unrolled bit-0x2 poll shape already
     * evidenced at 0x011098: `lw a4,0(s0); andi a4,a4,0x2; beqz a4,<retry>`.
     * The old value's bit 0x2 was clear, so the poll never saw its exit
     * condition. Report that bit set on the healthy path instead, dropping
     * the previously unevidenced low bit rather than keep a second guess
     * beside a now-evidenced one.
     */
    /*
     * This same word also carries a second, independent poll bit: live-
     * traced from the driver object pulled off the retained disk (not the
     * older 6.5-base release this project analysed first), mgras_hq4_
     * hiwater_intr -- called from MgrasPcxSwap during a buffer swap -- runs
     * its own short, ten-iteration local poll of this offset's bit 0x10
     * (`lw t9,0(s2); andi t9,t9,0x10; beqz t9,<retry>`) before it will
     * treat the FIFO as healthy. Bit 0x10 was never modelled, so that poll
     * always exhausted its ten retries and fell into the same watchdog-
     * arming fallback MgrasFlushHQ3Poll's own timeout does (writing
     * SGI_MGRAS_HQ_ENABLE's bit 0x10, see that macro's header comment) --
     * on every single call, deterministically, independent of the FIFO-
     * status fix above. Reporting the bit set on the healthy path is the
     * same "immediate readiness, no modelled delay" approximation already
     * used for bit 0x2 above.
     *
     * CONTRACT CHANGE, needs independent review: this register used to
     * report s->error (folded into bit 31) instead of its own healthy
     * bits whenever this device's sticky error flag was set, from any
     * cause anywhere on the device -- not just a genuine FIFO-related
     * fault. Root-caused live as part of the SGI_MGRAS_SCHED_ARG_4
     * (0x040070) investigation (docs/plans/si-graphics-widget-wiring-
     * handoff-20260916.md, "gfxinit clears GE download live, then
     * MgrasCtxsw timeout"): an unrelated unsupported access anywhere on
     * this device silently made every later, unrelated poll of this
     * register report "FIFO busy" until something explicitly acknowledged
     * the error via 0x011048. This device already has an independent,
     * dedicated way to surface "an error occurred" -- the sticky error
     * flag drives its own pending source (SI_PENDING_ERROR) and widget-
     * error interrupt delivery (si_error, si_deliver_widget_error) --
     * so overloading this status register's own healthy bits with it was
     * never necessary and actively broke every unrelated consumer.
     * Reporting the healthy bits unconditionally here does not change
     * whether an error is reported at all, only where.
     */
    case 0x020000: return 0x12;
    /*
     * MgrasReset writes zero here before polling it, live-traced once the
     * DAC load above stopped blocking that far: with no write case, the
     * write took the sticky unsupported-access path, which set the same
     * error flag the read case above reports through bit 31 -- so the
     * driver's own clearing write was what poisoned the very next read of
     * the readiness bit it was about to poll for. Accepting the write as a
     * no-op, the same treatment already given to 0x011098's write side,
     * lets the read case's existing healthy-path answer stand.
     */
    /*
     * MgrasFlushHQ3Poll's own second poll mode, live-traced from the actual
     * driver object pulled off the retained disk (mgras.o, not the older
     * 6.5-base release this project analysed first): when its own third
     * argument is zero, it reads this offset's low byte and requires
     * `caller_threshold >= (value & 0xff)` (`sltu`/`beqz`) before it will
     * stop retrying. Both live call sites that take this branch --
     * MgrasCtxsw's own MgrasFlushHQ3 call and MgrasPcxSwap's second one --
     * pass a threshold of zero, so this field has to read back exactly 0 to
     * satisfy the poll; MgrasPcxSwap's other two calls and MgrasCtxsw's own
     * take the sibling DMA_BUSY poll at 0x020200 instead, already always
     * zero.
     *
     * This offset is also the guest-visible read of this device's direct-
     * command FIFO occupancy. si_command now applies every state and
     * execute record to the persistent register file immediately, with no
     * modelled queueing depth, so occupancy is always zero -- not "31
     * units of available capacity", an earlier design note this pass's own
     * evidence trail settled as wrong (docs/plans/si-graphics-widget-
     * wiring-handoff-20260916.md, "The FIFO-occupancy field, settled"):
     * every consumer in the disassembled driver (MgrasWriteBfifo,
     * MgrasPositionCursor, MgrasInitVC3, MgrasClearCursor, MgrasSetColors,
     * MgrasReset) treats the low bits as a pending programmed-write count
     * and waits for it to reach zero, which reporting zero already
     * satisfies unconditionally. This is the same "immediate readiness, no
     * modelled delay" approximation already used at 0x011098, 0x020000 and
     * 0x020100 above.
     */
    case 0x020008: return 0;
    /*
     * The kernel driver's own reset routine polls this offset's low five
     * bits, waiting for ((bits + 1) >> 1) < 8 -- i.e. the field settling to
     * 14 or below -- before it will continue. The value this case returned
     * before, 31 << 4, has its low five bits fixed at 16, which never
     * satisfies that test: once this driver code actually ran (only
     * possible after installing the retained kernel module, which no
     * earlier pass had done), it spun here forever. Reporting an
     * already-settled field is the same "immediate readiness, no modelled
     * delay" approximation already used at 0x011098 above.
     */
    case 0x020100: return 0;
    case 0x020200: return 0;
    /*
     * A status poll live-traced immediately before the existing 0x02c578
     * case: `andi t2,t2,0x8; beqz t2,<retry>`, requiring bit 0x8 set to
     * continue. This device's usual "immediate readiness, no modelled
     * delay" approximation, already used at 0x011098 and 0x020000/
     * 0x020100 above, applies the same way here.
     */
    /*
     * A readback data register, not simply a status word: readData/readCC
     * push a 64-bit FIFO "read" command here before reading this offset
     * for the answer, and MgrasSyncPPRDRAMs polls its bit 3. This constant
     * satisfies that poll, but the FIFO read-command encoding it should
     * really answer from was never decoded, so any consumer expecting a
     * specific readback value (MgrasInitCCR) gets this fixed constant
     * instead -- a known, labelled approximation, not evidence.
     */
    case 0x02c574: return 0x8;
    case 0x02c578: return 0x100;
    /*
     * Live-traced polling this register with `lbu` at its own base
     * address (not a 4-byte load): `lbu t0,...; andi t0,t0,0x8;
     * bnez t0,<continue>`. A big-endian 1-byte read at a register's
     * base address extracts the most significant byte of the 32-bit
     * value the memory core widens it from, so the bit this poll
     * checks has to live there, not in the low byte a 0x08 return
     * would put it in.
     */
    case 0x071208: return 0x08000000;
    case 0x071c88: return s->cmap_index;
    case 0x071f00: return 1;
    /* See the header comment on SGI_MGRAS_REPP_SYNC. */
    case SGI_MGRAS_REPP_SYNC: return 0x101;
    /*
     * The register the index port above selected, widened into bits
     * 31:16 the same way every other 2-byte read on this device is (see
     * the 0x071208 comment). Index 0x1f (config) additionally clears its
     * own revision bits on readback -- see the header comment on
     * SGI_MGRAS_VC3_INDEXDATA -- everything else echoes back what was
     * last written through the indexed write port unchanged.
     */
    case SGI_MGRAS_VC3_DATA: {
        uint16_t v = s->vc3_index < ARRAY_SIZE(s->vc3_reg) ?
                     s->vc3_reg[s->vc3_index] : 0;
        if (s->vc3_index == 0x1f) {
            v &= ~0x60; /* revision bits: report "VC3 A", revision 0 */
        }
        return (uint32_t)v << 16;
    }
    /*
     * The VC3 SRAM's own data port, read back through whatever address
     * the index port (SGI_MGRAS_VC3_INDEXDATA index 7) last set, auto-
     * incrementing after every access the same way the write side does.
     */
    case SGI_MGRAS_VC3_RAM: {
        uint16_t v = s->vc3_sram[s->vc3_ram_addr &
                                 (SGI_MGRAS_VC3_SRAM_WORDS - 1)];
        s->vc3_ram_addr++;
        return (uint32_t)v << 16;
    }
    case 0x130500: return s->entry;
    case 0x070d18: return s->cmap[s->cmap_index & 255];
    case 0x120400: return s->ready ? 0xf400 : 0;
    /*
     * See the GE port header comment: GE_READ_1 is the driver's compare
     * port, GE_READ_2 the discard port it reads first and never checks.
     * The read pointer is a unit index; GE_READ_1 yields flat word
     * 2*ptr - odd and advances the pointer, GE_READ_2 yields the pass's
     * other word from the same pair without advancing -- this device's own
     * approximation for what GE_READ_2 carries, since the driver never
     * validates it.
     */
    case SGI_MGRAS_GE_READ_1:
    case SGI_MGRAS_GE_READ_2: {
        uint32_t slot = s->ge_active_slot;
        int64_t idx = 2 * (int64_t)s->ge_read_ptr[slot] -
                      s->ge_read_odd[slot] +
                      (addr == SGI_MGRAS_GE_READ_2 ? 1 : 0);

        if (addr == SGI_MGRAS_GE_READ_1) {
            s->ge_read_ptr[slot]++;
        }
        if (idx < 0 || idx >= s->ge_words[slot]) {
            return 0;
        }
        return s->ge[slot][idx];
    }
    default:
        if (addr >= 0x050000 && addr < 0x050000 + sizeof(s->hq) && size == 4) {
            return s->hq[(addr - 0x050000) >> 2] & 0x00ffffff;
        }
        if (addr >= SGI_MGRAS_CONFIG_TABLE_BASE &&
            addr < SGI_MGRAS_CONFIG_TABLE_BASE + sizeof(s->config_table) &&
            size == 4) {
            return s->config_table[(addr - SGI_MGRAS_CONFIG_TABLE_BASE) >> 2];
        }
        if (addr >= SGI_MGRAS_FIFO_THRESHOLD_BASE &&
            addr < SGI_MGRAS_FIFO_THRESHOLD_BASE +
                   sizeof(s->fifo_threshold) &&
            size == 4) {
            return s->fifo_threshold[(addr - SGI_MGRAS_FIFO_THRESHOLD_BASE)
                                      >> 2];
        }
        si_error(s, addr, size, "unsupported read");
        return 0;
    }
}

static void si_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    SGIMgrasState *s = opaque;
    trace_sgi_mgras_mmio(addr, value, size, true);
    /*
     * See the header comment on SGI_MGRAS_DAC_LUT_DATA_1_LOW: a genuinely
     * misaligned 4-byte write, ahead of the generic alignment guard below
     * because that guard would otherwise reject it before the address
     * switch ever sees it.
     */
    if (addr == SGI_MGRAS_DAC_LUT_DATA_1_LOW && size == 4) {
        s->dac_lut_data_1_low = value;
        return;
    }
    if ((size != 4 && size != 8) || (addr & (size - 1))) {
        si_error(s, addr, size, "unsupported or unaligned write");
        return;
    }
    switch (addr) {
    case SGI_MGRAS_INT_DEST_UPPER: s->int_dest_upper = value; return;
    case SGI_MGRAS_INT_DEST_LOWER: s->int_dest_lower = value; return;
    case SGI_MGRAS_INT_DEST_UPPER_ALT: s->int_dest_upper = value; return;
    case SGI_MGRAS_INT_DEST_LOWER_ALT: s->int_dest_lower = value; return;
    case SGI_MGRAS_WIDGET_CONTROL: s->widget_control = value; return;
    case SGI_MGRAS_RESET_ARG: s->reset_arg = value; return;
    case SGI_MGRAS_BRINGUP_ARG_2: s->bringup_arg_2 = value; return;
    case SGI_MGRAS_BRINGUP_BLOCK_1: s->bringup_block[0] = value; return;
    case SGI_MGRAS_BRINGUP_BLOCK_2: s->bringup_block[1] = value; return;
    case SGI_MGRAS_BRINGUP_BLOCK_3: s->bringup_block[2] = value; return;
    case SGI_MGRAS_SCRATCH_COMMAND: s->scratch_command = value; return;
    /*
     * This offset also serves as the kernel HQ driver's own enable-clear
     * register later in the boot (SGI_MGRAS_HQ_ENABLE's header comment);
     * both effects apply to the same write.
     *
     * Root-caused live (docs/plans/si-graphics-widget-wiring-handoff-
     * 20260916.md, "gfxinit clears GE download live, then MgrasCtxsw
     * timeout"): mgras_hq4_lowater_intr is the only handler that ever
     * clears SI_HQ_ENABLE_LOWATER back out here, but this device never
     * cleared SI_PENDING_LOWATER to match -- only a full reset did. The
     * first arm/deliver cycle worked, but the second (MgrasFlushHQ3 arms
     * this bit routinely, not just on a timeout) found the pending bit
     * already stuck set from the first delivery, so si_update_irq's own
     * transition detector never saw a fresh low->high edge and never sent
     * a second packet; the driver's 15-second FIFO watchdog then ran
     * unopposed. Clearing the pending bit on the same 1->0 transition this
     * register already detects for SI_HQ_ENABLE_LOWATER makes the role
     * re-triggerable, matching the datasheet's 0x011050 row: only the
     * low-water handler clears the bit.
     */
    case SGI_MGRAS_BRINGUP_ARG_3:
        s->bringup_arg_3 = value;
        if ((value & s->hq_enable) & SI_HQ_ENABLE_LOWATER) {
            s->pending &= ~SI_PENDING_LOWATER;
        }
        s->hq_enable &= ~(uint32_t)value;
        si_update_irq(s);
        return;
    case SGI_MGRAS_VC3_ARG: s->vc3_arg = value; return;
    case SGI_MGRAS_RE_ARG: s->re_arg = value; return;
    case SGI_MGRAS_RESET_CONTROL: s->reset_control = value; return;
    case SGI_MGRAS_I2C_SCRATCH: s->i2c_scratch = value; return;
    case SGI_MGRAS_I2C_STROBE: return;
    case SGI_MGRAS_REPP_STATUS_1: s->repp_status[0] = value; return;
    /* Already the colour map index register; see the header comment. */
    case 0x071c88: return;
    case SGI_MGRAS_REPP_STATUS_3: s->repp_status[1] = value; return;
    case SGI_MGRAS_REPP_STATUS_4: s->repp_status[2] = value; return;
    case SGI_MGRAS_SYNC_STATUS: s->sync_status = value; return;
    case SGI_MGRAS_BRINGUP_FLAG: s->bringup_flag = value; return;
    case SGI_MGRAS_DAC_LUT_INDEX: s->dac_lut_index = value; return;
    case SGI_MGRAS_DAC_LUT_DATA_1: s->dac_lut_data[0] = value; return;
    case SGI_MGRAS_DAC_LUT_DATA_2: s->dac_lut_data[1] = value; return;
    case SGI_MGRAS_DAC_CONFIG: s->dac_config = value; return;
    case 0x011000:
        /*
         * MgrasStart writes configuration here before the first context
         * switch (retained startup evidence). MgrasCtxsw also rewrites it
         * on every later context switch, after entry selection, as part
         * of its own ordinary per-context HQ-register save/restore -- not
         * a protocol violation, live-traced against a real MgrasStart
         * hang this device's old post-selection rejection caused (see the
         * 0x011000 row in docs/datasheets/mgras-hq4.md). The written
         * value's fields are unresolved; retain it as opaque prefix state
         * and use only its presence to gate the first entry selection at
         * 0x130500.
         */
        s->prefix = value;
        s->prefix_written = true;
        return;
    case 0x011028:
        s->pending &= ~(uint32_t)value;
        if (value & SI_PENDING_FLAGS) s->first_ack = true;
        if (value & SI_PENDING_CONTEXT) s->second_ack = true;
        si_update_irq(s); return;
    /* The extra bits MgrasCtxsw writes here; see the read side's comment. */
    case 0x011020: s->hq_status_extra = value; return;
    case 0x011040: s->irq_mask = value; si_update_irq(s); return;
    case 0x011048: s->error = 0; s->pending &= ~SI_PENDING_ERROR; si_update_irq(s); return;
    case SGI_MGRAS_HQ_CONFIG_A: s->hq_config_a = value; return;
    case SGI_MGRAS_HQ_CONFIG_B: s->hq_config_b = value; return;
    case SGI_MGRAS_HQ_ENABLE:
        /*
         * See the header comment on SI_PENDING_LOWATER: a newly-set bit
         * 0x10 here is MgrasFlushHQ3's or mgras_hq4_hiwater_intr's own
         * signal that it is about to wait on this device's FIFO
         * status/low-water interrupt, live-traced from both routines.
         */
        if ((value & ~s->hq_enable) & SI_HQ_ENABLE_LOWATER) {
            s->pending |= SI_PENDING_LOWATER;
        }
        s->hq_enable = value;
        si_update_irq(s);
        return;
    case SGI_MGRAS_HQ_VECTOR_DEST_UPPER: s->hq_vector_dest_upper = value; return;
    case SGI_MGRAS_HQ_VECTOR_DEST_LOWER: s->hq_vector_dest_lower = value; return;
    case SGI_MGRAS_HQ_VECTORS: s->hq_vectors = value; return;
    case SGI_MGRAS_SCHED_ARG_1: s->sched_arg_1 = value; return;
    case SGI_MGRAS_SCHED_ARG_2: s->sched_arg_2 = value; return;
    case SGI_MGRAS_SCHED_ARG_3: s->sched_arg_3 = value; return;
    case SGI_MGRAS_SCHED_ARG_4: s->sched_arg_4 = value; return;
    /* OR sets bits, AND-NOT clears them; see the header comment on
     * SGI_MGRAS_HQ_FLAGS. */
    case SGI_MGRAS_HQ_FLAGS: s->hq_flags |= value; return;
    case SGI_MGRAS_HQ_FLAGS_CLEAR: s->hq_flags &= ~(uint32_t)value; return;
    /* Always written zero in the evidenced case; see the header comment. */
    case SGI_MGRAS_RETRACE_ACK: return;
    /* The generic bring-up step argument; see the read side's comment. */
    case 0x011098: return;
    /* The reset routine's own clearing write; see the read side's comment. */
    case 0x020000: return;
    case 0x130500:
        if (size != 4 || value != 0x4fc || !s->prefix_written) {
            si_error(s, addr, size, "invalid HQ entry"); return;
        }
        s->entry = value; s->entry_selected = true; s->protocol = 1;
        s->first_ack = s->second_ack = false;
        s->pending |= SI_PENDING_FLAGS; si_update_irq(s); return;
    case 0x120400:
        if (size == 4) {
            si_context_write(s, value);
        } else if (size == 8 && s->ready) {
            si_command(s, value);
        } else if (size == 8 && !s->entry_selected) {
            /*
             * Board bring-up code writes a constant doubleword here, live-
             * traced well before the guest ever selects an HQ entry at
             * 0x130500 -- unrelated incidental traffic on the same offset
             * this device's real context/command protocol uses, not a
             * premature command submission. Accepting it silently, the
             * same "opaque, no invented meaning" treatment given to other
             * undocumented registers, avoids raising the sticky error over
             * a write that has nothing to do with the protocol this
             * register otherwise enforces. Once an entry is selected the
             * stricter check below still applies, so a genuine mid-
             * protocol command submitted before this device reports ready
             * is still flagged.
             */
            return;
        } else {
            si_error(s, addr, size, "unsupported context width");
        }
        return;
    /*
     * The colour-map index: a 2-byte write, widened into bits 31:16 the
     * same way every other 2-byte access on this device is (see the
     * 0x071208 comment) -- the low-byte mask this case used before always
     * saw zero, so the index this register selected never actually moved.
     */
    case 0x070c30: s->cmap_index = (value >> 16) & 255; return;
    case 0x070d18: s->cmap[s->cmap_index & 255] = value; return;
    case 0x071f00: return;
    /*
     * The channel-select word the driver writes before each readback; see
     * the header comment on SGI_MGRAS_REPP_SYNC for why the read side does
     * not echo this back.
     */
    case SGI_MGRAS_REPP_SYNC: return;
    /*
     * The VC3's indexed register write port: top byte selects the
     * register (see the header comment), bits 23:8 carry the 16-bit
     * value. Index 7 does not select an ordinary register -- it loads the
     * SRAM address pointer SGI_MGRAS_VC3_RAM's data port uses -- so it is
     * handled separately from the general register file. Cursor position
     * (indices 2/3, already evidenced from MgrasPositionCursor before this
     * offset's full contract was understood), the cursor-enable bit
     * (index 0x1d bit 1) and the display-enable bit (index 0x1e bit 2)
     * additionally update this device's own named state for whatever
     * cursor/retrace code already reads it.
     */
    case SGI_MGRAS_VC3_INDEXDATA: {
        uint8_t index = value >> 24;
        uint16_t data16 = (value >> 8) & 0xffff;

        if (index == 7) {
            s->vc3_ram_addr = data16;
            return;
        }
        if (index >= ARRAY_SIZE(s->vc3_reg)) {
            return;
        }
        s->vc3_reg[index] = data16;
        switch (index) {
        case 0x02: s->cursor_x = data16; break;
        case 0x03: s->cursor_y = data16; break;
        case 0x1d: s->cursor_enabled = (data16 & 0x2) != 0; break;
        case 0x1e: {
            /*
             * A transition, not a level: si_gfx_update reruns the
             * front[]->display_buffer conversion or the blank fill only
             * when dirty, so a write that leaves the bit unchanged should
             * not force a redundant redraw.
             */
            bool enabled = (data16 & 0x4) != 0;
            if (enabled != s->display_enabled) {
                s->dirty = true;
            }
            s->display_enabled = enabled;
            break;
        }
        default: break;
        }
        return;
    }
    /* The VC3's register-select port; see the header comment. */
    case SGI_MGRAS_VC3_INDEX: s->vc3_index = value >> 24; return;
    /* The VC3 SRAM's own data port; see the header comment. */
    case SGI_MGRAS_VC3_RAM:
        s->vc3_sram[s->vc3_ram_addr & (SGI_MGRAS_VC3_SRAM_WORDS - 1)] =
            (value >> 16) & 0xffff;
        s->vc3_ram_addr++;
        return;
    default: break;
    }
    if (addr >= 0x050000 && addr < 0x050000 + sizeof(s->hq) && size == 4) {
        s->hq[(addr - 0x050000) >> 2] = value;
        s->hq_words = MAX(s->hq_words, ((addr - 0x050000) >> 2) + 1);
        return;
    }
    if (addr >= SGI_MGRAS_CONFIG_TABLE_BASE &&
        addr < SGI_MGRAS_CONFIG_TABLE_BASE + sizeof(s->config_table) &&
        size == 4) {
        s->config_table[(addr - SGI_MGRAS_CONFIG_TABLE_BASE) >> 2] = value;
        return;
    }
    if (addr >= SGI_MGRAS_FIFO_THRESHOLD_BASE &&
        addr < SGI_MGRAS_FIFO_THRESHOLD_BASE + sizeof(s->fifo_threshold) &&
        size == 4) {
        s->fifo_threshold[(addr - SGI_MGRAS_FIFO_THRESHOLD_BASE) >> 2] =
            value;
        return;
    }
    if (addr == SGI_MGRAS_GE_CTRL_A) {
        si_ge_control_write(s, SGI_MGRAS_GE_SLOT_A, value);
        return;
    }
    if (addr == SGI_MGRAS_GE_CTRL_B) {
        si_ge_control_write(s, SGI_MGRAS_GE_SLOT_B, value);
        return;
    }
    if (addr == SGI_MGRAS_GE_DATA_A) {
        si_ge_data_write(s, addr, size, SGI_MGRAS_GE_SLOT_A, value);
        return;
    }
    if (addr == SGI_MGRAS_GE_DATA_B) {
        si_ge_data_write(s, addr, size, SGI_MGRAS_GE_SLOT_B, value);
        return;
    }
    si_error(s, addr, size, "unsupported write");
}

static const MemoryRegionOps si_ops = {
    .read = si_read, .write = si_write, .endianness = DEVICE_BIG_ENDIAN,
    /*
     * The kernel driver's own I2C probe (see the 0x073c08/0x073c88 cases
     * below) does plain byte-wide loads and stores. min_access_size stayed
     * at 4 since this device's first commit, so every one of those byte
     * accesses violated .valid outright -- not something this device's own
     * unsupported-access path could log, since the memory core rejects an
     * out-of-range width before ever reaching si_read/si_write, and MIPS
     * turns that rejection into a real bus-error exception. Widening this
     * to 1 lets the memory core's own width-adjustment machinery satisfy a
     * byte access by read-modify-writing the aligned word si_read/si_write
     * already handle, so neither function needs its own byte-width logic.
     */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    /*
     * Without an explicit impl width, the memory core defaults to 4 and
     * silently splits every 8-byte access this device's own si_read/
     * si_write already handle (FIFO command submission, HQ/GE readback)
     * into two narrower callbacks -- corrupting the context recognizer's
     * width check and losing the low half of every direct command.
     */
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void si_reset(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    memset(s->front, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->front));
    memset(s->back, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->back));
    memset(s->wid, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    memset(s->hq, 0, sizeof(s->hq));
    for (int slot = 0; slot < 2; slot++) {
        memset(s->ge[slot], 0, sizeof(uint32_t) * SGI_MGRAS_GE_WORDS);
        s->ge_write_ptr[slot] = s->ge_read_ptr[slot] = s->ge_words[slot] = 0;
        s->ge_read_odd[slot] = s->ge_load_mode[slot] = false;
    }
    s->ge_active_slot = 0;
    memset(s->context, 0, sizeof(s->context)); memset(s->cursor, 0, sizeof(s->cursor));
    memset(s->rss, 0, sizeof(s->rss));
    /*
     * Two reset defaults for the direct-command register file, not zero
     * like every other entry: a fresh register file with logic op 0
     * (GXclear) or plane mask 0 (nothing writable) would silently turn a
     * driver's very first fill, issued before it ever programs either
     * register, into a no-op instead of a plain opaque copy. X11 GCs
     * default to GXcopy and an all-ones planemask (AllPlanes) for the same
     * reason; this device picks the same defaults for its own analogous
     * registers rather than leaving them at zero. The default *values*
     * (GXcopy, all-ones) are this device's own approximation, not
     * evidence for a real hardware reset value -- but which register and
     * bit position each one lives at is now live-traced: OpenBSD's public
     * driver (sys/arch/sgi/dev/impact.c, impact_attach's own "RSS
     * initialization") sets `COLORMASKLSBSA`/`COLORMASKLSBSB` to
     * 0xffffff and `COLORMASKMSBS` to 0 once, at attach, and never
     * touches any of the three again before a solid fill -- confirming
     * `COLORMASKLSBSA`, not `COLORMASKMSBS`, is the mask that actually
     * gates a 24-bit colour write (`COLORMASKMSBS` covers bits 31:24,
     * always zero in this device's own 24-bit colour values, so it can
     * never be what a real fill's visible output depends on).
     */
    s->rss[SI_REG_PP1FILLMODE] = 0x3 << 26;
    s->rss[SI_REG_COLORMASKLSBSA] = 0xffffffff;
    s->glyph_row = s->pio_x = s->pio_y = 0;
    s->cmd_token_class = s->cmd_token_reg = 0;
    s->cmd_token_words_remaining = s->cmd_token_payload_count = 0;
    memset(s->cmd_token_payload, 0, sizeof(s->cmd_token_payload));
    memset(s->vc3_reg, 0, sizeof(s->vc3_reg));
    memset(s->vc3_sram, 0, sizeof(s->vc3_sram));
    s->vc3_index = 0; s->vc3_ram_addr = 0;
    s->pending = s->irq_mask = s->error = s->context_words = 0;
    s->entry = s->protocol = s->prefix = 0; s->prefix_written = false;
    s->entry_selected = s->ready = false;
    s->display_enabled = s->cursor_enabled = false;
    /*
     * Reset turns the display off (above) and clears front[] (above), so
     * the next gfx_update needs to redraw regardless of whatever it last
     * showed -- the same "mark dirty" treatment si_command_execute and
     * the 0x1e display-enable write case give a real content change.
     */
    s->dirty = true;
    s->xmap_mode = 0x7a4; s->cmap_index = 0; s->retraces = 0;
    s->int_dest_upper = s->int_dest_lower = 0;
    s->widget_control = 0;
    s->reset_arg = s->reset_control = s->i2c_scratch = 0;
    memset(s->repp_status, 0, sizeof(s->repp_status));
    s->sync_status = 0;
    s->dac_lut_index = 0;
    memset(s->dac_lut_data, 0, sizeof(s->dac_lut_data));
    s->dac_config = 0;
    s->dac_lut_data_1_low = 0;
    memset(s->config_table, 0, sizeof(s->config_table));
    s->bringup_flag = 0;
    s->bringup_arg_2 = 0;
    memset(s->bringup_block, 0, sizeof(s->bringup_block));
    s->scratch_command = 0;
    s->bringup_arg_3 = 0;
    s->vc3_arg = 0;
    s->re_arg = 0;
    s->hq_status_extra = 0;
    s->hq_vector_dest_upper = s->hq_vector_dest_lower = s->hq_vectors = 0;
    s->hq_config_a = s->hq_config_b = s->hq_enable = 0;
    memset(s->fifo_threshold, 0, sizeof(s->fifo_threshold));
    s->sched_arg_1 = s->sched_arg_2 = s->sched_arg_3 = s->sched_arg_4 = 0;
    s->hq_flags = 0;
    /*
     * Reset does not retroactively unsend an already-delivered packet, so
     * this clears each role's transition-detector directly rather than
     * going through si_update_irq(), which would otherwise read the
     * asserted -> deasserted edge as one more packet to send per role.
     */
    s->lowater_asserted = s->general_asserted = s->retrace_asserted =
        s->error_asserted = false;
    si_update_irq(s);
    /*
     * The retrace timer models continuous vsync timing, not something this
     * device starts only once the guest enables the display -- si_retrace
     * itself already gates on display_enabled and no-ops otherwise, and
     * re-arms itself for the next period every time it runs. Nothing
     * previously gave it its first arm, so it never ran at all; this is
     * that first arm, the same period si_retrace uses for every one after.
     */
    timer_mod(s->retrace_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

/*
 * Convert front[] (SI_REG_PACKEDCOLOR's own 0x00BBGGRR packing -- see
 * si_draw_fill and docs/datasheets/mgras-hq4.md's 0x120400 row) into
 * display_buffer's PIXMAN_x8r8g8b8 (0x00RRGGBB): B and R swap byte
 * position, G is already in the right place. Called on whatever cadence
 * the active UI backend drives (the same pattern RAD4's rad4_gfx_update
 * uses, hw/display/psitech-rad4.c), so this only redoes the conversion,
 * or the blank fill, when dirty says something could actually be
 * different from what display_buffer already holds.
 *
 * A real VC3 blanks its output while the display-enable bit is clear
 * (SGI_MGRAS_VC3_INDEXDATA index 0x1e bit 2, SGIMgrasState.
 * display_enabled) rather than scanning out whatever front[] holds; this
 * device fills display_buffer black to match instead of showing stale or
 * partially-drawn content from before the guest enabled the display.
 */
static bool si_gfx_update(void *opaque)
{
    SGIMgrasState *s = opaque;

    if (!s->dirty) {
        return true;
    }
    if (!s->display_enabled) {
        memset(s->display_buffer, 0,
              SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->display_buffer));
    } else {
        for (int i = 0; i < SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT; i++) {
            uint32_t v = s->front[i];

            s->display_buffer[i] = ((v & 0xff) << 16) | (v & 0x00ff00) |
                                   ((v >> 16) & 0xff);
        }
    }
    qemu_console_update_full(s->con);
    s->dirty = false;
    return true;
}

static const GraphicHwOps si_gfx_ops = {
    .gfx_update = si_gfx_update,
};

/*
 * qemu_graphic_console_create() starts the console on a small placeholder
 * surface of its own; recognize this device's own surface by its backing
 * buffer, the same check RAD4's rad4_ensure_surface uses
 * (hw/display/psitech-rad4.c), so a second call from si_post_load is a
 * no-op once si_realize has already installed the real one.
 */
static void si_ensure_surface(SGIMgrasState *s)
{
    DisplaySurface *ds = qemu_console_surface(s->con);

    if (ds && surface_data(ds) == (uint8_t *)s->display_buffer) {
        return;
    }
    ds = qemu_create_displaysurface_from(SGI_MGRAS_WIDTH, SGI_MGRAS_HEIGHT,
                                         PIXMAN_x8r8g8b8,
                                         SGI_MGRAS_WIDTH * 4,
                                         (uint8_t *)s->display_buffer);
    qemu_console_set_surface(s->con, ds);
}

static void si_realize(DeviceState *dev, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    if (!s->fabric) {
        error_setg(errp, "sgi-mgras needs the fabric its interrupt packets "
                  "travel");
        return;
    }
    s->front = g_new0(uint32_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->back = g_new0(uint32_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->front_count = s->back_count = SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT;
    s->wid = g_new0(uint8_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->ge[SGI_MGRAS_GE_SLOT_A] = g_new0(uint32_t, SGI_MGRAS_GE_WORDS);
    s->ge[SGI_MGRAS_GE_SLOT_B] = g_new0(uint32_t, SGI_MGRAS_GE_WORDS);
    s->display_buffer = g_new0(uint32_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->con = qemu_graphic_console_create(dev, 0, &si_gfx_ops, s);
    memory_region_init_io(&s->regs, OBJECT(dev), &si_ops, s, "sgi-mgras.regs", SGI_MGRAS_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
    address_space_init(&s->fabric_as, s->fabric, "sgi-mgras.fabric");
    s->retrace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, si_retrace, s);
    si_reset(dev);
    si_ensure_surface(s);
}

static void si_unrealize(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    timer_del(s->retrace_timer); timer_free(s->retrace_timer);
    address_space_destroy(&s->fabric_as);
    qemu_graphic_console_close(s->con);
    g_free(s->front); g_free(s->back); g_free(s->wid);
    g_free(s->ge[SGI_MGRAS_GE_SLOT_A]); g_free(s->ge[SGI_MGRAS_GE_SLOT_B]);
    g_free(s->display_buffer);
}

/*
 * con and display_buffer are host-only (see the header comment on those
 * fields); neither is migrated. display_enabled and cursor_enabled are
 * likewise not migrated directly -- vc3_reg (already migrated) already
 * carries the same two bits this device derived them from at write time
 * (SGI_MGRAS_VC3_INDEXDATA's index 0x1e/0x1d cases), so re-deriving them
 * here on load avoids persisting the same fact twice. Forcing dirty true
 * regardless of any transmitted value means a migrated device always
 * redraws on its first post-migration gfx_update, the same treatment
 * si_reset already gives a fresh device.
 */
static int si_post_load(void *opaque, int version_id)
{
    SGIMgrasState *s = opaque;

    si_ensure_surface(s);
    s->display_enabled = (s->vc3_reg[0x1e] & 0x4) != 0;
    s->cursor_enabled = (s->vc3_reg[0x1d] & 0x2) != 0;
    s->dirty = true;
    return 0;
}

static const VMStateDescription vmstate_sgi_mgras = {
    .name = TYPE_SGI_MGRAS, .version_id = 1, .minimum_version_id = 1,
    .post_load = si_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(front_count, SGIMgrasState),
        VMSTATE_VARRAY_UINT32(front, SGIMgrasState, front_count, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(back_count, SGIMgrasState),
        VMSTATE_VARRAY_UINT32(back, SGIMgrasState, back_count, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32_ARRAY(hq, SGIMgrasState, 5120),
        VMSTATE_UINT32_ARRAY(context, SGIMgrasState, 63),
        VMSTATE_UINT32(context_words, SGIMgrasState), VMSTATE_UINT32(entry, SGIMgrasState),
        VMSTATE_UINT32(protocol, SGIMgrasState), VMSTATE_UINT64(prefix, SGIMgrasState),
        VMSTATE_BOOL(prefix_written, SGIMgrasState),
        VMSTATE_BOOL(entry_selected, SGIMgrasState), VMSTATE_BOOL(ready, SGIMgrasState),
        VMSTATE_UINT32(pending, SGIMgrasState), VMSTATE_UINT32(irq_mask, SGIMgrasState),
        VMSTATE_UINT32(error, SGIMgrasState), VMSTATE_UINT32(error_addr, SGIMgrasState),
        VMSTATE_UINT32(error_width, SGIMgrasState), VMSTATE_UINT32(cmap_index, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(cmap, SGIMgrasState, 256), VMSTATE_UINT64(retraces, SGIMgrasState),
        VMSTATE_UINT32(revision, SGIMgrasState), VMSTATE_UINT32(part, SGIMgrasState),
        VMSTATE_UINT32(manufacturer, SGIMgrasState),
        VMSTATE_UINT32(int_dest_upper, SGIMgrasState),
        VMSTATE_UINT32(int_dest_lower, SGIMgrasState),
        VMSTATE_UINT32(widget_control, SGIMgrasState),
        VMSTATE_UINT32(reset_arg, SGIMgrasState),
        VMSTATE_UINT32(reset_control, SGIMgrasState),
        VMSTATE_UINT32(i2c_scratch, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(repp_status, SGIMgrasState, 3),
        VMSTATE_UINT32(sync_status, SGIMgrasState),
        VMSTATE_UINT32(dac_lut_index, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(dac_lut_data, SGIMgrasState, 2),
        VMSTATE_UINT32(dac_config, SGIMgrasState),
        VMSTATE_UINT32(dac_lut_data_1_low, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(config_table, SGIMgrasState,
                             SGI_MGRAS_CONFIG_TABLE_WORDS),
        VMSTATE_UINT32(bringup_flag, SGIMgrasState),
        VMSTATE_UINT32(bringup_arg_2, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(bringup_block, SGIMgrasState, 3),
        VMSTATE_UINT64(scratch_command, SGIMgrasState),
        VMSTATE_UINT32(bringup_arg_3, SGIMgrasState),
        VMSTATE_UINT32(vc3_arg, SGIMgrasState),
        VMSTATE_UINT32(re_arg, SGIMgrasState),
        VMSTATE_UINT32(hq_status_extra, SGIMgrasState),
        VMSTATE_UINT32(hq_vector_dest_upper, SGIMgrasState),
        VMSTATE_UINT32(hq_vector_dest_lower, SGIMgrasState),
        VMSTATE_UINT32(hq_vectors, SGIMgrasState),
        VMSTATE_UINT32(hq_config_a, SGIMgrasState),
        VMSTATE_UINT32(hq_config_b, SGIMgrasState),
        VMSTATE_UINT32(hq_enable, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(fifo_threshold, SGIMgrasState,
                             SGI_MGRAS_FIFO_THRESHOLD_WORDS),
        VMSTATE_UINT32(sched_arg_1, SGIMgrasState),
        VMSTATE_UINT32(sched_arg_2, SGIMgrasState),
        VMSTATE_UINT32(sched_arg_3, SGIMgrasState),
        VMSTATE_UINT32(sched_arg_4, SGIMgrasState),
        VMSTATE_UINT32(hq_flags, SGIMgrasState),
        VMSTATE_BOOL(lowater_asserted, SGIMgrasState),
        VMSTATE_BOOL(general_asserted, SGIMgrasState),
        VMSTATE_BOOL(retrace_asserted, SGIMgrasState),
        VMSTATE_BOOL(error_asserted, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(rss, SGIMgrasState, 0x200),
        VMSTATE_UINT32(glyph_row, SGIMgrasState),
        VMSTATE_UINT32(pio_x, SGIMgrasState),
        VMSTATE_UINT32(pio_y, SGIMgrasState),
        VMSTATE_UINT32(cmd_token_class, SGIMgrasState),
        VMSTATE_UINT32(cmd_token_reg, SGIMgrasState),
        VMSTATE_UINT32(cmd_token_words_remaining, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(cmd_token_payload, SGIMgrasState,
                             SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS),
        VMSTATE_UINT32(cmd_token_payload_count, SGIMgrasState),
        VMSTATE_UINT16_ARRAY(vc3_reg, SGIMgrasState, 32),
        VMSTATE_UINT8(vc3_index, SGIMgrasState),
        VMSTATE_UINT16(vc3_ram_addr, SGIMgrasState),
        VMSTATE_UINT16_ARRAY(vc3_sram, SGIMgrasState,
                             SGI_MGRAS_VC3_SRAM_WORDS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property si_properties[] = {
    DEFINE_PROP_UINT32("revision", SGIMgrasState, revision, 0),
    DEFINE_PROP_UINT32("part", SGIMgrasState, part, 0xc003),
    DEFINE_PROP_UINT32("manufacturer", SGIMgrasState, manufacturer, 0x02aa),
    DEFINE_PROP_LINK("fabric", SGIMgrasState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("fabric-base", SGIMgrasState, fabric_base, 0),
    DEFINE_PROP_UINT32("fabric-widget-shift", SGIMgrasState,
                       fabric_widget_shift, 0),
};

/*
 * Host-only pixel introspection for qtests: nothing here is guest-visible.
 * Framebuffer readback is deliberately unsupported on the MMIO interface
 * (si-one-mode-functional-approximation.md, "Explicit exclusions"), so a
 * qtest that wants to check a rendered pixel has no other way to see one.
 * x-test-pixel-x/-y set the coordinate a following read of x-test-pixel
 * reports, mirroring the port-then-register pattern hw/misc/sgi-router-test.c
 * already uses for the same kind of host-only, index-then-read access.
 */
static void si_test_get_pixel(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (s->test_pixel_x < SGI_MGRAS_WIDTH && s->test_pixel_y < SGI_MGRAS_HEIGHT) {
        value = s->front[s->test_pixel_y * SGI_MGRAS_WIDTH + s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only display-surface introspection, the same port-then-register
 * shape as x-test-pixel above and reusing its x/y coordinate properties
 * rather than adding a second pair: forces a synchronous gfx_update (the
 * same call qxl.c's qxl_hw_update makes to qemu_console_hw_update, the
 * only in-tree precedent for driving one outside the UI's own refresh
 * loop) so a read observes this device's own current dirty/display-enable
 * state rather than whatever display_buffer happened to hold from a
 * previous, possibly stale, update.
 */
static void si_test_get_display_pixel(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    qemu_console_hw_update(s->con);
    if (s->test_pixel_x < SGI_MGRAS_WIDTH && s->test_pixel_y < SGI_MGRAS_HEIGHT) {
        value = s->display_buffer[s->test_pixel_y * SGI_MGRAS_WIDTH +
                                  s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

static void si_instance_init(Object *obj)
{
    SGIMgrasState *s = SGI_MGRAS(obj);

    object_property_add_uint32_ptr(obj, "x-test-pixel-x", &s->test_pixel_x,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-pixel-y", &s->test_pixel_y,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-pixel", "uint32", si_test_get_pixel,
                        NULL, NULL, NULL);
    object_property_add(obj, "x-test-display-pixel", "uint32",
                        si_test_get_display_pixel, NULL, NULL, NULL);
    object_property_add_uint64_ptr(obj, "x-test-last-interrupt-addr",
                                   &s->test_last_interrupt_addr,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "x-test-last-interrupt-value",
                                   &s->test_last_interrupt_value,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-error", &s->error,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-error-addr",
                                   &s->error_addr, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-pending", &s->pending,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-irq-mask", &s->irq_mask,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-hq-enable", &s->hq_enable,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-int-dest-upper",
                                   &s->int_dest_upper, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-int-dest-lower",
                                   &s->int_dest_lower, OBJ_PROP_FLAG_READ);
}

static void si_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = si_realize; dc->unrealize = si_unrealize; device_class_set_legacy_reset(dc, si_reset);
    dc->vmsd = &vmstate_sgi_mgras; dc->desc = "SGI MGRAS SI functional endpoint";
    device_class_set_props(dc, si_properties);
}

static const TypeInfo si_types[] = {{ .name = TYPE_SGI_MGRAS, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIMgrasState), .instance_init = si_instance_init,
    .class_init = si_class_init }};
DEFINE_TYPES(si_types)
