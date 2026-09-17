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
 * never disarmed and IRIX panics roughly 15 seconds after any code path
 * (gfxinit, the X server) opens this board. This device tracks no real FIFO
 * occupancy to know when a genuine low-water transition would happen, so it
 * satisfies the wait immediately on the arming write rather than never --
 * the same "immediate readiness, no modelled delay" approximation already
 * used elsewhere in this file (see si_update_irq and the SGI_MGRAS_HQ_ENABLE
 * write case).
 *
 * This delivers the interrupt (confirmed live: the pending bit sets and a
 * packet goes out), but does not by itself stop the panic: MgrasCtxsw also
 * restores this same word's bit 0x10 unconditionally on every context
 * switch, independent of any poll, and a live boot still reaches the
 * identical panic with this mechanism and both known poll-timeout fixes
 * (0x020000, 0x020008) applied together. See docs/datasheets/mgras-hq4.md's
 * HQ interrupt/config block row for the still-open remainder: the interrupt
 * fires but mgras_hq4_lowater_intr does not appear to run in time, most
 * likely because this device has no per-role interrupt vector (0x011070
 * stays opaque) and IRIX's real dispatch may key off it.
 */
#define SI_PENDING_LOWATER 0x00000004
#define SI_HQ_ENABLE_LOWATER 0x10

/* Where the Hub's interrupt-modify register sits; data bit 8 selects raise. */
#define SI_HUB_INT_MODIFY_SET 0x100
#define SI_INT_DEST_WIDGET_SHIFT 16
#define SI_INT_DEST_WIDGET_BITS 4
#define SI_INT_DEST_VECTOR_SHIFT 24

/*
 * Compose and send one crosstalk interrupt packet from the widget-level
 * destination pair, the same wire form hw/pci-host/sgi-bridge.c uses
 * (sgi_bridge_deliver_interrupt/compose_xtalk): a write to the target's
 * interrupt-modify register carrying the vector and a raise/lower bit. The
 * destination is guest-programmed and unvalidated hardware fact beyond that
 * generic widget-header layout (see the header comment on int_dest_upper).
 */
static void si_deliver_interrupt(SGIMgrasState *s, bool set)
{
    unsigned int widget = (s->int_dest_upper >> SI_INT_DEST_WIDGET_SHIFT) &
                          ((1u << SI_INT_DEST_WIDGET_BITS) - 1);
    uint64_t addr = ((uint64_t)(s->int_dest_upper & 0xffff) << 32) |
                    s->int_dest_lower;
    uint8_t vector = s->int_dest_upper >> SI_INT_DEST_VECTOR_SHIFT;
    uint64_t value = vector | (set ? SI_HUB_INT_MODIFY_SET : 0);
    hwaddr where;

    if (!s->int_dest_upper && !s->int_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: interrupt %s with no "
                      "destination programmed; no packet sent\n",
                      set ? "raised" : "lowered");
        return;
    }
    if (s->fabric_widget_shift) {
        if (widget >= (1u << SI_INT_DEST_WIDGET_BITS) ||
            addr >> s->fabric_widget_shift) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: packet fabric cannot "
                          "encode widget %u address 0x%" PRIx64 "\n", widget,
                          addr);
            return;
        }
        where = ((uint64_t)widget << s->fabric_widget_shift) | addr;
    } else {
        where = s->fabric_base + addr;
    }
    /* Host-only qtest introspection; see the header comment on the fields. */
    s->test_last_interrupt_addr = where;
    s->test_last_interrupt_value = value;
    address_space_stq(&s->fabric_as, where, value, MEMTXATTRS_UNSPECIFIED,
                      NULL);
}

static void si_update_irq(SGIMgrasState *s)
{
    /*
     * SI_PENDING_LOWATER bypasses the shared top-level mask: this device
     * has no evidence for which bit of the guest's own irq_mask write
     * (0x011040) real hardware assigns to the status/low-water role (see
     * the header comment on SI_PENDING_LOWATER), while the second-level
     * enable bit that gates it (SGI_MGRAS_HQ_ENABLE's bit 0x10) is
     * live-traced from two independent driver routines. Gating delivery on
     * that bit instead, rather than on a guessed top-level mask bit, is
     * sufficient to satisfy the driver's own wait.
     */
    uint32_t active = (s->pending & s->irq_mask) |
                       (s->pending & SI_PENDING_LOWATER);
    bool level = !!active;

    /* The chip sends on a transition, not while a level is held. */
    if (level != s->irq_asserted) {
        s->irq_asserted = level;
        si_deliver_interrupt(s, level);
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

static void si_draw_fill(SGIMgrasState *s)
{
    uint32_t xy0 = s->command[0x147], xy1 = s->command[0x148];
    uint32_t color = s->command[0x140] & 0x00ffffff;
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            s->front[y * SGI_MGRAS_WIDTH + x] = color;
        }
    }
}

static void si_command(SGIMgrasState *s, uint64_t word)
{
    uint32_t high = word >> 32;
    uint32_t selector;
    uint32_t value = word;

    /*
     * MgrasStart submits two HQ-local configuration words around the
     * context switch (the GE and RE population counts), live-traced with
     * a zero unit field in the high half rather than a real command
     * selector: 0x0000ff04 and 0x00008c04. Neither is ever read back, so
     * this accepts them as opaque configuration traffic instead of routing
     * them through the entry-register selector check below, which was
     * designed before this part of the sequence was understood and
     * otherwise flags them as an "unknown command selector".
     */
    if (high == 0x0000ff04 || high == 0x00008c04) {
        return;
    }

    selector = (word >> 34) & 0xffffff;
    if (selector < 0x60001 || ((selector - 0x60001) & 0xf)) {
        si_error(s, 0x120400, 8, "unknown command selector");
        return;
    }
    uint32_t reg = (selector - 0x60001) >> 4;
    if (reg >= ARRAY_SIZE(s->command)) {
        si_error(s, 0x120400, 8, "command register out of range");
        return;
    }
    if (s->fifo_count == SGI_MGRAS_FIFO_DEPTH) {
        si_error(s, 0x120400, 8, "command FIFO full");
        return;
    }
    s->command[reg] = value;
    s->command_valid[reg] = 1;
    s->fifo_count++;
    if (reg == 0x47) {
        if (!s->command_valid[0x147] || !s->command_valid[0x148] ||
            !s->command_valid[0x140]) {
            si_error(s, 0x120400, 8, "incomplete fill command");
            return;
        }
        si_draw_fill(s);
        s->fifo_count = 0;
    }
}

/*
 * One GE11 engine slot's control port. 0x200000 resets both the write
 * pointer the load loop fills and the read pointer the verify loop drains;
 * a value with bit 31 set arms readback by resetting only the read pointer,
 * matching the driver's own "control 0x80000000 | count" step right before
 * it starts reading back. Every other control word (the 0x40038 mode flag
 * the driver pairs with a 0xd1 data write, the GE reset pulse this device
 * doesn't model) is accepted as opaque state, the same treatment this
 * device already gives undocumented control registers elsewhere.
 */
static void si_ge_control_write(SGIMgrasState *s, unsigned int slot,
                                uint32_t value)
{
    s->ge_active_slot = slot;
    if (value == 0x200000) {
        s->ge_write_ptr[slot] = 0;
        s->ge_read_ptr[slot] = 0;
    } else if (value & 0x80000000) {
        s->ge_read_ptr[slot] = 0;
    }
}

/*
 * One GE11 engine slot's data port: each write appends one microcode word
 * to that slot's own buffer, matching the driver's three-writes-per-triple
 * load loop (traced in MgrasDownloadGE11Ucode). The 0xd1 marker word the
 * driver writes just before re-arming readback lands here too, but the
 * control write that follows it resets the pointer back to zero, so it
 * never reaches the buffer the verify loop actually reads from.
 */
static void si_ge_data_write(SGIMgrasState *s, hwaddr addr, unsigned size,
                             unsigned int slot, uint32_t value)
{
    uint32_t ptr = s->ge_write_ptr[slot];

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
    case SGI_MGRAS_BANK_STATUS_1: return s->bank_status_1;
    case SGI_MGRAS_BANK_STATUS_2: return s->bank_status_2;
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
     */
    case 0x020000: return s->error ? 0x80000000 : 0x12;
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
     * s->fifo_count is this device's own FIFO occupancy accounting for
     * si_command's overflow check (SGI_MGRAS_FIFO_DEPTH above): it advances
     * on every accepted command word and only returns to zero once a fill
     * command (register 0x47) completes, so a diagnostic or setup sequence
     * that submits a word without ever completing a fill leaves it stuck
     * nonzero -- exactly what live-tracing a boot that reaches gfxinit's
     * own HQ4 widget diagnostic showed: MgrasCtxsw's poll timed out and
     * armed the FIFO-status watchdog (SGI_MGRAS_HQ_ENABLE's bit 0x10 above),
     * which this device has no way to disarm again since it tracks no real
     * FIFO drain over time, panicking IRIX roughly 15 seconds later. This
     * offset is the only guest-visible read of that accounting, so
     * reporting it separately as always-ready is the same "immediate
     * readiness, no modelled delay" approximation already used at 0x011098,
     * 0x020000 and 0x020100 above, without weakening the overflow check
     * s->fifo_count still serves inside si_command.
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
    case SGI_MGRAS_GE_READ_1:
    case SGI_MGRAS_GE_READ_2: {
        uint32_t slot = s->ge_active_slot;
        uint32_t ptr = s->ge_read_ptr[slot];

        if (ptr >= s->ge_words[slot]) {
            return 0;
        }
        s->ge_read_ptr[slot] = ptr + 1;
        return s->ge[slot][ptr];
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
     */
    case SGI_MGRAS_BRINGUP_ARG_3:
        s->bringup_arg_3 = value;
        s->hq_enable &= ~(uint32_t)value;
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
         * switch (retained startup evidence). The written value's fields
         * are unresolved; retain it as opaque prefix state and use only
         * its presence to gate the first entry selection at 0x130500.
         */
        if (s->entry_selected) {
            si_error(s, addr, size, "prefix write after entry selection");
            return;
        }
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
    case SGI_MGRAS_BANK_STATUS_1: s->bank_status_1 = value; return;
    case SGI_MGRAS_BANK_STATUS_2: s->bank_status_2 = value; return;
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
        case 0x1e: s->display_enabled = (data16 & 0x4) != 0; break;
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
    if (addr == 0x120400 && size == 8) { si_command(s, value); return; }
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
    }
    s->ge_active_slot = 0;
    memset(s->context, 0, sizeof(s->context)); memset(s->cursor, 0, sizeof(s->cursor));
    memset(s->command_valid, 0, sizeof(s->command_valid));
    memset(s->vc3_reg, 0, sizeof(s->vc3_reg));
    memset(s->vc3_sram, 0, sizeof(s->vc3_sram));
    s->vc3_index = 0; s->vc3_ram_addr = 0;
    s->pending = s->irq_mask = s->error = s->context_words = 0;
    s->entry = s->protocol = s->prefix = 0; s->prefix_written = false;
    s->entry_selected = s->ready = false;
    s->display_enabled = s->cursor_enabled = false; s->fifo_count = 0;
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
    s->sched_arg_1 = s->sched_arg_2 = s->sched_arg_3 = 0;
    s->bank_status_1 = s->bank_status_2 = 0;
    /*
     * Reset does not retroactively unsend an already-delivered packet, so
     * this clears the transition-detector directly rather than going
     * through si_update_irq(), which would otherwise read the asserted ->
     * deasserted edge as one more packet to send.
     */
    s->irq_asserted = false;
    si_update_irq(s);
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
    memory_region_init_io(&s->regs, OBJECT(dev), &si_ops, s, "sgi-mgras.regs", SGI_MGRAS_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
    address_space_init(&s->fabric_as, s->fabric, "sgi-mgras.fabric");
    s->retrace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, si_retrace, s);
    si_reset(dev);
}

static void si_unrealize(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    timer_del(s->retrace_timer); timer_free(s->retrace_timer);
    address_space_destroy(&s->fabric_as);
    g_free(s->front); g_free(s->back); g_free(s->wid);
    g_free(s->ge[SGI_MGRAS_GE_SLOT_A]); g_free(s->ge[SGI_MGRAS_GE_SLOT_B]);
}

static const VMStateDescription vmstate_sgi_mgras = {
    .name = TYPE_SGI_MGRAS, .version_id = 1, .minimum_version_id = 1,
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
        VMSTATE_UINT32(bank_status_1, SGIMgrasState),
        VMSTATE_UINT32(bank_status_2, SGIMgrasState),
        VMSTATE_BOOL(irq_asserted, SGIMgrasState),
        VMSTATE_UINT16(fifo_count, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(command, SGIMgrasState, 0x200),
        VMSTATE_UINT8_ARRAY(command_valid, SGIMgrasState, 0x200),
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

static void si_instance_init(Object *obj)
{
    SGIMgrasState *s = SGI_MGRAS(obj);

    object_property_add_uint32_ptr(obj, "x-test-pixel-x", &s->test_pixel_x,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-pixel-y", &s->test_pixel_y,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-pixel", "uint32", si_test_get_pixel,
                        NULL, NULL, NULL);
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
    object_property_add_uint16_ptr(obj, "x-test-fifo-count", &s->fifo_count,
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
