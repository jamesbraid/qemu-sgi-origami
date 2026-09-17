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
    uint32_t active = s->pending & s->irq_mask;
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
    si_error(s, 0x120400, 4, "context write after completion");
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
    uint32_t selector = (word >> 34) & 0xffffff;
    uint32_t value = word;
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
    case SGI_MGRAS_WIDGET_CONTROL: return s->widget_control;
    case SGI_MGRAS_RESET_ARG: return s->reset_arg;
    case SGI_MGRAS_BRINGUP_ARG_2: return s->bringup_arg_2;
    case SGI_MGRAS_BRINGUP_BLOCK_1: return s->bringup_block[0];
    case SGI_MGRAS_BRINGUP_BLOCK_2: return s->bringup_block[1];
    case SGI_MGRAS_BRINGUP_BLOCK_3: return s->bringup_block[2];
    case SGI_MGRAS_SCRATCH_COMMAND: return s->scratch_command;
    case SGI_MGRAS_BRINGUP_ARG_3: return s->bringup_arg_3;
    case SGI_MGRAS_VC3_ARG: return s->vc3_arg;
    case SGI_MGRAS_VC3_SRAM: return s->vc3_sram;
    case SGI_MGRAS_RE_ARG: return s->re_arg;
    case SGI_MGRAS_RESET_CONTROL: return s->reset_control;
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
    case 0x011020: return s->pending;
    case 0x011040: return s->irq_mask;
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
    case 0x020000: return s->error ? 0x80000000 : 0x2;
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
    case 0x020008: return s->fifo_count;
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
    case 0x072038: return s->cursor_control;
    case 0x130500: return s->entry;
    case 0x070d18: return s->cmap[s->cmap_index & 255];
    case 0x120400: return s->ready ? 0xf400 : 0;
    case 0x58230:
    case 0x5822c:
        return s->ge_addr < s->ge_words ? s->ge[s->ge_addr] : 0;
    default:
        if (addr >= 0x050000 && addr < 0x050000 + sizeof(s->hq) && size == 4) {
            return s->hq[(addr - 0x050000) >> 2] & 0x00ffffff;
        }
        if (addr >= SGI_MGRAS_CONFIG_TABLE_BASE &&
            addr < SGI_MGRAS_CONFIG_TABLE_BASE + sizeof(s->config_table) &&
            size == 4) {
            return s->config_table[(addr - SGI_MGRAS_CONFIG_TABLE_BASE) >> 2];
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
    case SGI_MGRAS_WIDGET_CONTROL: s->widget_control = value; return;
    case SGI_MGRAS_RESET_ARG: s->reset_arg = value; return;
    case SGI_MGRAS_BRINGUP_ARG_2: s->bringup_arg_2 = value; return;
    case SGI_MGRAS_BRINGUP_BLOCK_1: s->bringup_block[0] = value; return;
    case SGI_MGRAS_BRINGUP_BLOCK_2: s->bringup_block[1] = value; return;
    case SGI_MGRAS_BRINGUP_BLOCK_3: s->bringup_block[2] = value; return;
    case SGI_MGRAS_SCRATCH_COMMAND: s->scratch_command = value; return;
    case SGI_MGRAS_BRINGUP_ARG_3: s->bringup_arg_3 = value; return;
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
    case 0x011040: s->irq_mask = value; si_update_irq(s); return;
    case 0x011048: s->error = 0; s->pending &= ~SI_PENDING_ERROR; si_update_irq(s); return;
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
    case 0x070c30: s->cmap_index = value & 255; return;
    case 0x070d18: s->cmap[s->cmap_index & 255] = value; return;
    case 0x071f00: return;
    /*
     * The channel-select word the driver writes before each readback; see
     * the header comment on SGI_MGRAS_REPP_SYNC for why the read side does
     * not echo this back.
     */
    case SGI_MGRAS_REPP_SYNC: return;
    case 0x072038:
        /*
         * MgrasPositionCursor tags an ordered X word (0x02) and Y word
         * (0x03) in the top byte, with the kernel-adjusted coordinate in
         * bits 23:8; the kernel does the origin subtraction and +31 before
         * this write, so the device only ever sees the final value. Any
         * other top byte is the cursor control/enable word, which combines
         * with 0x1d000000. Matching the tag exactly (rather than by masked
         * bits) keeps a Y write from being misread as a control write: 0x03
         * and 0x1d share bit 24.
         */
        if ((value >> 24) == 0x02) {
            s->cursor_x = (value >> 8) & 0xffff;
        } else if ((value >> 24) == 0x03) {
            s->cursor_y = (value >> 8) & 0xffff;
        } else {
            s->cursor_control = value;
            s->cursor_enabled = ((value & 0x1d000000) == 0x1d000000);
        }
        return;
    case SGI_MGRAS_VC3_SRAM: s->vc3_sram = value; s->display_enabled = true; return;
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
    if (addr == 0x120400 && size == 8) { si_command(s, value); return; }
    if (addr == 0x58044 || addr == 0x5804c) { s->ge_addr = value % (59392 * 3); return; }
    if (addr == 0x58040 || addr == 0x58048) {
        if (s->ge_addr < 59392 * 3) {
            s->ge[s->ge_addr++] = value;
            s->ge_words = MAX(s->ge_words, s->ge_addr);
        }
        else si_error(s, addr, size, "GE upload overflow");
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

/*
 * The host's own screendump command needs a real console; without one it
 * reports "There is no console to take a screendump from" even though the
 * device already keeps a complete framebuffer in front[]. The console's
 * surface is created directly over front[]'s own storage below, so this
 * callback only has to say "something in it may have changed" -- there is
 * no separate pixel copy to perform or dirty region to track.
 */
static bool si_gfx_update(void *opaque)
{
    SGIMgrasState *s = opaque;
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps si_gfx_ops = {
    .gfx_update = si_gfx_update,
};

static void si_reset(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    memset(s->front, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->front));
    memset(s->back, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->back));
    memset(s->wid, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    memset(s->hq, 0, sizeof(s->hq)); memset(s->ge, 0, sizeof(uint32_t) * 59392 * 3);
    memset(s->context, 0, sizeof(s->context)); memset(s->cursor, 0, sizeof(s->cursor));
    memset(s->command_valid, 0, sizeof(s->command_valid));
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
    s->vc3_sram = 0;
    s->re_arg = 0;
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
    s->ge = g_new0(uint32_t, 59392 * 3);
    memory_region_init_io(&s->regs, OBJECT(dev), &si_ops, s, "sgi-mgras.regs", SGI_MGRAS_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
    address_space_init(&s->fabric_as, s->fabric, "sgi-mgras.fabric");
    s->con = qemu_graphic_console_create(dev, 0, &si_gfx_ops, s);
    qemu_console_set_surface(s->con,
        qemu_create_displaysurface_from(SGI_MGRAS_WIDTH, SGI_MGRAS_HEIGHT,
                                        PIXMAN_x8r8g8b8,
                                        SGI_MGRAS_WIDTH * sizeof(uint32_t),
                                        (uint8_t *)s->front));
    s->retrace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, si_retrace, s);
    si_reset(dev);
}

static void si_unrealize(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    timer_del(s->retrace_timer); timer_free(s->retrace_timer);
    address_space_destroy(&s->fabric_as);
    /*
     * Closes the console and drops its surface before front[] is freed;
     * that surface wraps front[]'s storage without owning it (no pixman
     * destroy function was registered), so this ordering is required, not
     * just tidy -- freeing front[] first would leave a dangling surface.
     */
    qemu_graphic_console_close(s->con);
    g_free(s->front); g_free(s->back); g_free(s->wid); g_free(s->ge);
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
        VMSTATE_UINT32(vc3_sram, SGIMgrasState),
        VMSTATE_UINT32(re_arg, SGIMgrasState),
        VMSTATE_BOOL(irq_asserted, SGIMgrasState),
        VMSTATE_UINT16(fifo_count, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(command, SGIMgrasState, 0x200),
        VMSTATE_UINT8_ARRAY(command_valid, SGIMgrasState, 0x200),
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
