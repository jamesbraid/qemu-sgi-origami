/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded functional SI graphics endpoint. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/misc/sgi-mgras-display.h"
#include "hw/misc/sgi-mgras-raster.h"
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
/* FIFO occupancy is not modeled, so low-water readiness is immediate. */
#define SI_PENDING_LOWATER 0x00000004
#define SI_HQ_ENABLE_LOWATER 0x10

/* Data bit 8 selects assertion in the interrupt-modify write. */
#define SI_HUB_INT_MODIFY_SET 0x100
#define SI_INT_DEST_WIDGET_SHIFT 16
#define SI_INT_DEST_WIDGET_BITS 4
#define SI_INT_DEST_VECTOR_SHIFT 24

/* The HQ vector destination uses bits 27:24 for the target widget. */
#define SI_HQ_VECTOR_WIDGET_SHIFT 24
#define SI_HQ_VECTOR_WIDGET_BITS 4

/* Byte offsets within the stored HQ vector word. */
#define SI_HQ_VECTOR_LOWATER_SHIFT 0
#define SI_HQ_VECTOR_GENERAL_SHIFT 8
#define SI_HQ_VECTOR_RETRACE_SHIFT 16

#define SI_MGRAS_CONTEXT_COMMAND 0x120400

/* Send an interrupt-modify write to the selected fabric address. */
static void si_send_packet(SGIMgrasState *s, hwaddr where, uint8_t vector,
                           bool set)
{
    uint64_t value = vector | (set ? SI_HUB_INT_MODIFY_SET : 0);

    /* Retain the packet for host-only qtest inspection. */
    s->test_last_interrupt_addr = where;
    s->test_last_interrupt_value = value;
    address_space_stq(&s->fabric_as, where, value, MEMTXATTRS_UNSPECIFIED,
                      NULL);
}

/* Compose the fabric address without folding the widget into its payload. */
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

/* Widget errors use the generic destination and its embedded vector. */
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

/* HQ roles share one destination and select separate stored vectors. */
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
    /* The low-water top-level mask bit is unknown and remains unsupported. */
    bool lowater = !!(s->pending & SI_PENDING_LOWATER);
    bool general = !!(s->pending & s->irq_mask &
                      (SI_PENDING_FLAGS | SI_PENDING_CONTEXT));
    bool retrace = !!(s->pending & s->irq_mask & SI_PENDING_RETRACE);
    bool error = !!(s->pending & s->irq_mask & SI_PENDING_ERROR);

    uint8_t lowater_vector = s->hq_vectors >> SI_HQ_VECTOR_LOWATER_SHIFT;
    uint8_t general_vector = s->hq_vectors >> SI_HQ_VECTOR_GENERAL_SHIFT;
    uint8_t retrace_vector = s->hq_vectors >> SI_HQ_VECTOR_RETRACE_SHIFT;

    /* This approximation sends one packet for each modeled transition. */
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
    /* Post-completion words remain opaque because their grammar is unknown. */
}

/*
 * This approximation stores uploaded GE words without interpreting them.
 * Other control values end the upload, and bit 31 rewinds its read pointer.
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

/* Data writes append only while this approximation considers upload active. */
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

static uint64_t si_read_internal(SGIMgrasState *s, hwaddr addr, unsigned size)
{
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        si_error(s, addr, size, "unsupported read width");
        return 0;
    }
    if ((addr == 0x071c88 && size != 1) ||
        (addr == SGI_MGRAS_XMAP_MAIN_MODE && size != 4) ||
        (addr == 0x070c30 && size != 2) ||
        (addr == 0x070d18 && size != 4)) {
        si_error(s, addr, size, "unsupported register read width");
        return 0;
    }
    if (size == 1 && addr != SGI_MGRAS_I2C_SCRATCH && addr != 0x071208 &&
        addr != 0x071c88) {
        si_error(s, addr, size, "unsupported byte read");
        return 0;
    }
    if (size == 2 && addr != SGI_MGRAS_VC3_DATA &&
        addr != SGI_MGRAS_VC3_RAM) {
        si_error(s, addr, size, "unsupported halfword read");
        return 0;
    }
    switch (addr) {
    case 0x000004:
        return (s->revision << 28) | (s->part << 12) |
               (s->manufacturer << 1) | 1;
    case SGI_MGRAS_INT_DEST_UPPER: return s->int_dest_upper;
    case SGI_MGRAS_INT_DEST_LOWER: return s->int_dest_lower;
    /* The alternate offsets alias the same destination state. */
    case SGI_MGRAS_INT_DEST_UPPER_ALT: return s->int_dest_upper;
    case SGI_MGRAS_INT_DEST_LOWER_ALT: return s->int_dest_lower;
    case SGI_MGRAS_WIDGET_CONTROL: return s->widget_control;
    /* The unresolved prefix state is stored and echoed without decoding it. */
    case 0x011000: return s->prefix;
    case SGI_MGRAS_RESET_ARG: return s->reset_arg;
    /* Fixed functional-approximation population value. */
    case SGI_MGRAS_RESET_CONTROL: return 0x0f000000;
    /* Fixed functional-approximation inventory value. */
    case SGI_MGRAS_BOARD_INVENTORY: return 0x21000000;
    /* Unresolved revision fields use zero as a functional approximation. */
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
    /* XMAP selection and per-visual modes are an approximation. */
    case SGI_MGRAS_XMAP_MAIN_MODE: return s->xmap_mode[s->xmap_index];
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
    /* Keep unresolved status bits separate from modeled pending sources. */
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
    /* Both offsets expose the same stored flag word. */
    case SGI_MGRAS_HQ_FLAGS: return s->hq_flags;
    case SGI_MGRAS_HQ_FLAGS_CLEAR: return s->hq_flags;
    /* Retrace status content is unsupported. */
    case SGI_MGRAS_RETRACE_STATUS_1: return 0;
    case SGI_MGRAS_RETRACE_STATUS_2: return 0;
    /* Generic widget readiness is an immediate functional approximation. */
    case 0x011098: return 0x2;
    /* FIFO readiness is immediate. Exact thresholds remain unknown. */
    case 0x020000: return 0x12;
    /* Commands execute synchronously, so modeled FIFO occupancy is zero. */
    case 0x020008: return 0;
    /* Bus and DMA activity are not modeled and report idle. */
    case 0x020100: return 0;
    case 0x020200: return 0;
    /* Readback grammar is unsupported. These are fixed approximations. */
    case 0x02c574: return 0x8;
    case 0x02c578: return 0x100;
    /* Preserve the approximated status bit across supported access widths. */
    case 0x071208: return size == 1 ? 0x08 : 0x08000000;
    case 0x071c88: return s->xmap_index << 2;
    case 0x071f00: return 1;
    /* Fixed functional-approximation synchronization value. */
    case SGI_MGRAS_REPP_SYNC: return 0x101;
    /* VC3 values are stored and echoed; revision bits are approximate. */
    case SGI_MGRAS_VC3_DATA: {
        uint16_t v = s->vc3_index < ARRAY_SIZE(s->vc3_reg) ?
                     s->vc3_reg[s->vc3_index] : 0;
        if (s->vc3_index == 0x1f) {
            v &= ~0x60; /* Functional-approximation revision value. */
        }
        return size == 2 ? v : (uint32_t)v << 16;
    }
    /* VC3 SRAM accesses auto-increment the stored address. */
    case SGI_MGRAS_VC3_RAM: {
        uint16_t v = s->vc3_sram[s->vc3_ram_addr &
                                 (SGI_MGRAS_VC3_SRAM_WORDS - 1)];
        s->vc3_ram_addr++;
        return size == 2 ? v : (uint32_t)v << 16;
    }
    case 0x130500: return s->entry;
    case 0x070d18: {
        uint32_t address = (s->cmap_selector & 0x1f) * 256 +
                           ((s->cmap_selector >> 13) & 7) * 32 +
                           s->cmap_position;

        return s->cmap[address];
    }
    case 0x120400: return s->ready ? 0xf400 : 0;
    /* GE readback order is a functional approximation over opaque uploads. */
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

static uint64_t si_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIMgrasState *s = opaque;
    uint64_t value = si_read_internal(s, addr, size);
    uint64_t sequence = s->trace_sequence++;

    trace_sgi_mgras_mmio(sequence, addr, value, size, false);
    if (addr != SI_MGRAS_CONTEXT_COMMAND && addr != SGI_MGRAS_SCRATCH_COMMAND) {
        trace_sgi_mgras_control_mmio(sequence, addr, value, size, false);
    }
    return value;
}

static void si_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    SGIMgrasState *s = opaque;
    uint64_t sequence = s->trace_sequence++;

    trace_sgi_mgras_mmio(sequence, addr, value, size, true);
    if (addr != SI_MGRAS_CONTEXT_COMMAND && addr != SGI_MGRAS_SCRATCH_COMMAND) {
        trace_sgi_mgras_control_mmio(sequence, addr, value, size, true);
    }
    /* Handle this supported misaligned port before the alignment check. */
    if (addr == SGI_MGRAS_DAC_LUT_DATA_1_LOW && size == 4) {
        s->dac_lut_data_1_low = value;
        return;
    }
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        (addr & (size - 1))) {
        si_error(s, addr, size, "unsupported or unaligned write");
        return;
    }
    if ((addr == 0x071c88 && size != 1) ||
        (addr == SGI_MGRAS_XMAP_MAIN_MODE && size != 4) ||
        (addr == 0x070c30 && size != 2) ||
        (addr == 0x070d18 && size != 4)) {
        si_error(s, addr, size, "unsupported register write width");
        return;
    }
    if (size == 1 && addr != SGI_MGRAS_I2C_SCRATCH &&
        addr != SGI_MGRAS_I2C_STROBE && addr != SGI_MGRAS_VC3_INDEX &&
        addr != 0x071c88) {
        si_error(s, addr, size, "unsupported byte write");
        return;
    }
    if (size == 2 && addr != 0x070c30 && addr != SGI_MGRAS_VC3_RAM) {
        si_error(s, addr, size, "unsupported halfword write");
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
    /* Clear modeled low-water state when its enable is cleared. */
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
    /* Per-visual mode storage and scanout interpretation are approximate. */
    case SGI_MGRAS_XMAP_MAIN_MODE:
        s->xmap_mode[s->xmap_index] = value;
        s->dirty = true;
        return;
    case SGI_MGRAS_RESET_CONTROL: s->reset_control = value; return;
    case SGI_MGRAS_I2C_SCRATCH: s->i2c_scratch = value; return;
    case SGI_MGRAS_I2C_STROBE: return;
    case SGI_MGRAS_REPP_STATUS_1: s->repp_status[0] = value; return;
    case 0x071c88:
        s->xmap_index = (value >> 2) & 0x1f;
        return;
    case SGI_MGRAS_REPP_STATUS_3: s->repp_status[1] = value; return;
    case SGI_MGRAS_REPP_STATUS_4: s->repp_status[2] = value; return;
    case SGI_MGRAS_SYNC_STATUS: s->sync_status = value; return;
    case SGI_MGRAS_BRINGUP_FLAG: s->bringup_flag = value; return;
    case SGI_MGRAS_DAC_LUT_INDEX: s->dac_lut_index = value; return;
    case SGI_MGRAS_DAC_LUT_DATA_1: s->dac_lut_data[0] = value; return;
    case SGI_MGRAS_DAC_LUT_DATA_2: s->dac_lut_data[1] = value; return;
    case SGI_MGRAS_DAC_CONFIG: s->dac_config = value; return;
    case 0x011000:
        /* Retain the unresolved prefix and use only its presence. */
        s->prefix = value;
        s->prefix_written = true;
        return;
    case 0x011028:
        s->pending &= ~(uint32_t)value;
        if (value & SI_PENDING_FLAGS) s->first_ack = true;
        if (value & SI_PENDING_CONTEXT) s->second_ack = true;
        si_update_irq(s); return;
    /* Store unresolved status bits separately from pending sources. */
    case 0x011020: s->hq_status_extra = value; return;
    case 0x011040: s->irq_mask = value; si_update_irq(s); return;
    case 0x011048: s->error = 0; s->pending &= ~SI_PENDING_ERROR; si_update_irq(s); return;
    case SGI_MGRAS_HQ_CONFIG_A: s->hq_config_a = value; return;
    case SGI_MGRAS_HQ_CONFIG_B: s->hq_config_b = value; return;
    case SGI_MGRAS_HQ_ENABLE:
        /* Immediate readiness raises the modeled low-water source. */
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
    /* The set port ORs bits. The clear port removes them. */
    case SGI_MGRAS_HQ_FLAGS: s->hq_flags |= value; return;
    case SGI_MGRAS_HQ_FLAGS_CLEAR: s->hq_flags &= ~(uint32_t)value; return;
    /* Store this unresolved scheduling argument without interpreting it. */
    case SGI_MGRAS_RETRACE_ACK: return;
    /* The generic readiness argument has no modeled side effect. */
    case 0x011098: return;
    /* The approximated status register accepts a clearing write. */
    case 0x020000: return;
    case 0x130500:
        if (size != 4 || value != 0x4fc || !s->prefix_written) {
            si_error(s, addr, size, "invalid HQ entry"); return;
        }
        s->entry = value; s->entry_selected = true; s->protocol = 1;
        s->first_ack = s->second_ack = false;
        /* A new transaction restarts the context word count. */
        s->context_words = 0;
        s->pending |= SI_PENDING_FLAGS; si_update_irq(s); return;
    case 0x120400:
        if (size == 4) {
            si_context_write(s, value);
        } else if (size == 8 && s->ready) {
            si_raster_command(s, value);
        } else if (size == 8 && !s->entry_selected) {
            /* Pre-selection traffic is opaque and has no modeled effect. */
            return;
        } else {
            si_error(s, addr, size, "unsupported context width");
        }
        return;
    /* Target-first selector decoding is a retained-trace approximation. */
    case 0x070c30:
        s->cmap_selector = value;
        s->cmap_position = 0;
        return;
    case 0x070d18: {
        uint32_t address = (s->cmap_selector & 0x1f) * 256 +
                           ((s->cmap_selector >> 13) & 7) * 32 +
                           s->cmap_position;

        s->cmap[address] = value;
        s->cmap_position = (s->cmap_position + 1) & 0x1f;
        s->dirty = true;
        return;
    }
    case 0x071f00: return;
    /* The unresolved synchronization selector is accepted but not stored. */
    case SGI_MGRAS_REPP_SYNC: return;
    /* Index 7 selects SRAM. Other indices update the stored VC3 state. */
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
        /* Cursor changes require a scanout refresh in the synchronous model. */
        case 0x01:
            s->dirty = true;
            break;
        case 0x02: s->cursor_x = data16; s->dirty = true; break;
        case 0x03: s->cursor_y = data16; s->dirty = true; break;
        case 0x1d: {
            bool enabled = (data16 & 0x2) != 0;
            if (enabled != s->cursor_enabled) {
                s->dirty = true;
            }
            s->cursor_enabled = enabled;
            break;
        }
        case 0x1e: {
            /* Redraw only when display enable changes. */
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
    /* Store the selected VC3 register. */
    case SGI_MGRAS_VC3_INDEX:
        s->vc3_index = size == 1 ? value : value >> 24;
        return;
    /* Store VC3 SRAM data and advance the address. */
    case SGI_MGRAS_VC3_RAM:
        s->vc3_sram[s->vc3_ram_addr & (SGI_MGRAS_VC3_SRAM_WORDS - 1)] =
            (size == 2 ? value : value >> 16) & 0xffff;
        s->vc3_ram_addr++;
        s->dirty = true;
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
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    /* Preserve 64-bit FIFO words instead of splitting them into callbacks. */
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void si_reset_enter(Object *obj, ResetType type)
{
    SGIMgrasClass *mc = SGI_MGRAS_GET_CLASS(obj);
    SGIMgrasState *s = SGI_MGRAS(obj);

    if (mc->parent_phases.enter) {
        mc->parent_phases.enter(obj, type);
    }
    memset(s->front, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->front));
    memset(s->back, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->back));
    memset(s->wid, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    memset(s->indexed, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    memset(s->hq, 0, sizeof(s->hq));
    for (int slot = 0; slot < 2; slot++) {
        memset(s->ge[slot], 0, sizeof(uint32_t) * SGI_MGRAS_GE_WORDS);
        s->ge_write_ptr[slot] = s->ge_read_ptr[slot] = s->ge_words[slot] = 0;
        s->ge_read_odd[slot] = s->ge_load_mode[slot] = false;
    }
    s->ge_active_slot = 0;
    memset(s->context, 0, sizeof(s->context)); memset(s->cursor, 0, sizeof(s->cursor));
    memset(s->rss, 0, sizeof(s->rss));
    /* Logic-operation and plane-mask reset values are approximations. */
    s->rss[0x161] = 0x3 << 26;
    s->rss[0x163] = 0xffffffff;
    s->glyph_row = s->pio_x = s->pio_y = 0;
    s->cmd_token_class = s->cmd_token_reg = 0;
    s->cmd_token_words_remaining = s->cmd_token_payload_count = 0;
    memset(s->cmd_token_payload, 0, sizeof(s->cmd_token_payload));
    memset(s->vc3_reg, 0, sizeof(s->vc3_reg));
    memset(s->vc3_sram, 0, sizeof(s->vc3_sram));
    s->vc3_index = 0; s->vc3_ram_addr = 0;
    s->pending = s->irq_mask = s->error = s->context_words = 0;
    s->entry = s->protocol = s->prefix = 0; s->prefix_written = false;
    s->entry_selected = s->first_ack = s->second_ack = s->ready = false;
    s->display_enabled = s->cursor_enabled = false;
    /* Redraw because reset changes display state and pixel storage. */
    s->dirty = true;
    for (int i = 0; i < SGI_MGRAS_XMAP_ENTRIES; i++) {
        s->xmap_mode[i] = 0x7a4;
    }
    s->xmap_index = 0;
    s->cmap_selector = 0;
    s->cmap_position = 0;
    memset(s->cmap, 0, sizeof(s->cmap));
    s->retraces = 0;
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
    s->lowater_asserted = s->general_asserted = s->retrace_asserted =
        s->error_asserted = false;
}

static void si_reset_hold(Object *obj, ResetType type)
{
    SGIMgrasClass *mc = SGI_MGRAS_GET_CLASS(obj);
    SGIMgrasState *s = SGI_MGRAS(obj);

    if (mc->parent_phases.hold) {
        mc->parent_phases.hold(obj, type);
    }
    si_update_irq(s);
}

static void si_reset_exit(Object *obj, ResetType type)
{
    SGIMgrasClass *mc = SGI_MGRAS_GET_CLASS(obj);
    SGIMgrasState *s = SGI_MGRAS(obj);

    if (mc->parent_phases.exit) {
        mc->parent_phases.exit(obj, type);
    }
    /* Retrace timing is a fixed-period approximation. */
    timer_mod(s->retrace_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
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
    s->indexed = g_new0(uint8_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->ge[SGI_MGRAS_GE_SLOT_A] = g_new0(uint32_t, SGI_MGRAS_GE_WORDS);
    s->ge[SGI_MGRAS_GE_SLOT_B] = g_new0(uint32_t, SGI_MGRAS_GE_WORDS);
    s->display_buffer = g_new0(uint32_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->con = qemu_graphic_console_create(dev, 0, &si_mgras_display_ops, s);
    memory_region_init_io(&s->regs, OBJECT(dev), &si_ops, s, "sgi-mgras.regs", SGI_MGRAS_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
    address_space_init(&s->fabric_as, s->fabric, "sgi-mgras.fabric");
    s->retrace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, si_retrace, s);
    device_cold_reset(dev);
    si_mgras_display_ensure_surface(s);
}

static void si_unrealize(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    timer_del(s->retrace_timer); timer_free(s->retrace_timer);
    address_space_destroy(&s->fabric_as);
    qemu_graphic_console_close(s->con);
    g_free(s->front); g_free(s->back); g_free(s->wid); g_free(s->indexed);
    g_free(s->ge[SGI_MGRAS_GE_SLOT_A]); g_free(s->ge[SGI_MGRAS_GE_SLOT_B]);
    g_free(s->display_buffer);
}

/* Rebuild host-only display state from migrated VC3 state and redraw it. */
static int si_post_load(void *opaque, int version_id)
{
    SGIMgrasState *s = opaque;

    if (s->ge_active_slot >= ARRAY_SIZE(s->ge) ||
        s->ge_write_ptr[0] > s->ge_words[0] ||
        s->ge_write_ptr[1] > s->ge_words[1] ||
        s->context_words > ARRAY_SIZE(s->context) ||
        s->cmd_token_reg >= ARRAY_SIZE(s->rss) ||
        s->cmd_token_payload_count > ARRAY_SIZE(s->cmd_token_payload) ||
        (s->cmd_token_class == SGI_MGRAS_CMD_TOKEN_DISCARD &&
         s->cmd_token_words_remaining >
         SGI_MGRAS_CMD_TOKEN_MAX_DISCARD_WORDS) ||
        (s->cmd_token_class != SGI_MGRAS_CMD_TOKEN_DISCARD &&
         s->cmd_token_words_remaining > ARRAY_SIZE(s->cmd_token_payload)) ||
        s->pio_x > SGI_MGRAS_WIDTH || s->pio_y > SGI_MGRAS_HEIGHT ||
        s->xmap_index >= SGI_MGRAS_XMAP_ENTRIES ||
        s->cmap_position >= 32) {
        return -EINVAL;
    }

    si_mgras_display_ensure_surface(s);
    s->display_enabled = (s->vc3_reg[0x1e] & 0x4) != 0;
    s->cursor_enabled = (s->vc3_reg[0x1d] & 0x2) != 0;
    s->dirty = true;
    return 0;
}

static bool si_front_count_valid(void *opaque, int version_id)
{
    SGIMgrasState *s = opaque;

    return s->front_count == SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT;
}

static bool si_back_count_valid(void *opaque, int version_id)
{
    SGIMgrasState *s = opaque;

    return s->back_count == SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT;
}

static bool si_ge_lengths_valid(void *opaque, int version_id)
{
    SGIMgrasState *s = opaque;

    return s->ge_words[0] <= SGI_MGRAS_GE_WORDS &&
           s->ge_words[1] <= SGI_MGRAS_GE_WORDS;
}

/*
 * Origami's unversioned machines support migration between identical builds
 * only. Change both version_id and minimum_version_id when represented state
 * changes.
 */
static const VMStateDescription vmstate_sgi_mgras = {
    .name = TYPE_SGI_MGRAS, .version_id = 4, .minimum_version_id = 4,
    .post_load = si_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(front_count, SGIMgrasState),
        VMSTATE_VALIDATE("front plane length is fixed", si_front_count_valid),
        VMSTATE_VARRAY_UINT32(front, SGIMgrasState, front_count, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(back_count, SGIMgrasState),
        VMSTATE_VALIDATE("back plane length is fixed", si_back_count_valid),
        VMSTATE_VARRAY_UINT32(back, SGIMgrasState, back_count, 0,
                              vmstate_info_uint32, uint32_t),
        /* The WID plane has the same dimensions as the front color plane. */
        VMSTATE_VARRAY_UINT32(wid, SGIMgrasState, front_count, 0,
                              vmstate_info_uint8, uint8_t),
        VMSTATE_VARRAY_UINT32(indexed, SGIMgrasState, front_count, 0,
                              vmstate_info_uint8, uint8_t),
        VMSTATE_UINT32_ARRAY(hq, SGIMgrasState, 5120),
        VMSTATE_UINT32_V(hq_words, SGIMgrasState, 2),
        VMSTATE_UINT32_V(hq_hash, SGIMgrasState, 2),
        VMSTATE_UINT32_ARRAY_V(ge_words, SGIMgrasState, 2, 2),
        VMSTATE_VALIDATE("GE upload lengths are in range", si_ge_lengths_valid),
        VMSTATE_VARRAY_UINT32(ge[0], SGIMgrasState, ge_words[0], 2,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_VARRAY_UINT32(ge[1], SGIMgrasState, ge_words[1], 2,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32_ARRAY_V(ge_write_ptr, SGIMgrasState, 2, 2),
        VMSTATE_UINT32_ARRAY_V(ge_read_ptr, SGIMgrasState, 2, 2),
        VMSTATE_UINT32_V(ge_active_slot, SGIMgrasState, 2),
        VMSTATE_BOOL_ARRAY_V(ge_read_odd, SGIMgrasState, 2, 2),
        VMSTATE_BOOL_ARRAY_V(ge_load_mode, SGIMgrasState, 2, 2),
        VMSTATE_UINT32_V(ge_hash, SGIMgrasState, 2),
        VMSTATE_UINT32_ARRAY(context, SGIMgrasState, 63),
        VMSTATE_UINT32(context_words, SGIMgrasState), VMSTATE_UINT32(entry, SGIMgrasState),
        VMSTATE_UINT32(protocol, SGIMgrasState), VMSTATE_UINT64(prefix, SGIMgrasState),
        VMSTATE_BOOL(prefix_written, SGIMgrasState),
        VMSTATE_BOOL(entry_selected, SGIMgrasState),
        VMSTATE_BOOL_V(first_ack, SGIMgrasState, 2),
        VMSTATE_BOOL_V(second_ack, SGIMgrasState, 2),
        VMSTATE_BOOL(ready, SGIMgrasState),
        VMSTATE_UINT32(pending, SGIMgrasState), VMSTATE_UINT32(irq_mask, SGIMgrasState),
        VMSTATE_UINT32(error, SGIMgrasState), VMSTATE_UINT32(error_addr, SGIMgrasState),
        VMSTATE_UINT32(error_width, SGIMgrasState),
        VMSTATE_UINT16(cmap_selector, SGIMgrasState),
        VMSTATE_UINT8(cmap_position, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(cmap, SGIMgrasState, SGI_MGRAS_CMAP_ENTRIES),
        VMSTATE_UINT64(retraces, SGIMgrasState),
        VMSTATE_UINT32_V(cursor_x, SGIMgrasState, 2),
        VMSTATE_UINT32_V(cursor_y, SGIMgrasState, 2),
        VMSTATE_BOOL_V(display_enabled, SGIMgrasState, 3),
        VMSTATE_BOOL_V(cursor_enabled, SGIMgrasState, 3),
        VMSTATE_UINT8_ARRAY_V(cursor, SGIMgrasState, 256, 3),
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
        VMSTATE_UINT8(xmap_index, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(xmap_mode, SGIMgrasState,
                             SGI_MGRAS_XMAP_ENTRIES),
        VMSTATE_TIMER_PTR_V(retrace_timer, SGIMgrasState, 2),
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

static bool si_test_get_first_ack(Object *obj, Error **errp)
{
    return SGI_MGRAS(obj)->first_ack;
}

static bool si_test_get_second_ack(Object *obj, Error **errp)
{
    return SGI_MGRAS(obj)->second_ack;
}

/* Host-only qtest access to native pixel storage. */
static void si_test_get_pixel(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized && s->test_pixel_x < SGI_MGRAS_WIDTH &&
        s->test_pixel_y < SGI_MGRAS_HEIGHT) {
        value = s->front[s->test_pixel_y * SGI_MGRAS_WIDTH + s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

/* Update scanout before exposing a display pixel to qtests. */
static void si_test_get_display_pixel(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized) {
        qemu_console_hw_update(s->con);
    }
    if (DEVICE(obj)->realized && s->test_pixel_x < SGI_MGRAS_WIDTH &&
        s->test_pixel_y < SGI_MGRAS_HEIGHT) {
        value = s->display_buffer[s->test_pixel_y * SGI_MGRAS_WIDTH +
                                  s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

/* Expose the current model WID tag for replay diagnostics. */
static void si_test_get_wid(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized && s->test_pixel_x < SGI_MGRAS_WIDTH &&
        s->test_pixel_y < SGI_MGRAS_HEIGHT) {
        value = s->wid[s->test_pixel_y * SGI_MGRAS_WIDTH + s->test_pixel_x];
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
    object_property_add(obj, "x-test-wid", "uint32", si_test_get_wid,
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
    object_property_add_bool(obj, "x-test-first-ack",
                             si_test_get_first_ack, NULL);
    object_property_add_bool(obj, "x-test-second-ack",
                             si_test_get_second_ack, NULL);
    object_property_add_uint32_ptr(obj, "x-test-int-dest-upper",
                                   &s->int_dest_upper, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-int-dest-lower",
                                   &s->int_dest_lower, OBJ_PROP_FLAG_READ);
}

static void si_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SGIMgrasClass *mc = SGI_MGRAS_CLASS(oc);

    dc->realize = si_realize;
    dc->unrealize = si_unrealize;
    resettable_class_set_parent_phases(rc, si_reset_enter, si_reset_hold,
                                       si_reset_exit, &mc->parent_phases);
    dc->vmsd = &vmstate_sgi_mgras; dc->desc = "SGI MGRAS SI functional endpoint";
    device_class_set_props(dc, si_properties);
}

static const TypeInfo si_types[] = {{ .name = TYPE_SGI_MGRAS, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIMgrasState), .class_size = sizeof(SGIMgrasClass),
    .instance_init = si_instance_init,
    .class_init = si_class_init }};
DEFINE_TYPES(si_types)
