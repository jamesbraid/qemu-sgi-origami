/* SPDX-License-Identifier: BSD-3-Clause */
/* Bounded functional SI graphics endpoint. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/misc/sgi-mgras-display.h"
#include "hw/misc/sgi-mgras-gl.h"
#include "hw/misc/sgi-mgras-raster.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/mips/sgi/xio-widget.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "trace.h"

#define MGRAS_PENDING_FLAGS 0x00080000
#define MGRAS_PENDING_CONTEXT 0x10000000
#define MGRAS_PENDING_RETRACE 0x00000001
#define MGRAS_PENDING_ERROR 0x00000002
/* FIFO occupancy is not modeled, so low-water readiness is immediate. */
#define MGRAS_PENDING_LOWATER 0x00000004
#define MGRAS_HQ_ENABLE_LOWATER 0x10

/*
 * VC3 register 0x1e: bit 2 enables the display, bit 0 enables the retrace
 * interrupt. The IRIX 6.5.30 kernel's MgrasEnableRetraceInterrupts sets bit
 * 0 through 0x072038; the VC2's control register uses the same two bits
 * (Linux include/video/newport.h, VC2_CTRL_EVIRQ and VC2_CTRL_EVIDEO).
 */
#define MGRAS_VC3_CONTROL 0x1e
#define MGRAS_VC3_CONTROL_VINTR 0x1
#define MGRAS_VC3_CONTROL_DISPLAY 0x4


/* The HQ vector destination uses bits 27:24 for the target widget. */
#define MGRAS_HQ_VECTOR_WIDGET_SHIFT 24
#define MGRAS_HQ_VECTOR_WIDGET_BITS 4

/* Byte offsets within the stored HQ vector word. */
#define MGRAS_HQ_VECTOR_LOWATER_SHIFT 0
#define MGRAS_HQ_VECTOR_GENERAL_SHIFT 8
#define MGRAS_HQ_VECTOR_RETRACE_SHIFT 16

#define MGRAS_CONTEXT_COMMAND 0x120400

/* VC3 register 7 loads, and reads back, the SRAM address. */
#define MGRAS_VC3_RAM_ADDR 0x07

/*
 * XMAP configuration register (0x071d00) selectors, as xmap_index decodes
 * the index bytes SGI's IDE writes: 4 selects the revision, 8 the FIFO
 * capacity.
 */
#define MGRAS_XMAP_CONFIG_REVISION 1
#define MGRAS_XMAP_CONFIG_FIFO_CAPACITY 2

/* DAC register addresses, as written to the DAC address halfword. */
#define MGRAS_DAC_REG_ID 0x0300
#define MGRAS_DAC_REG_REVISION 0x0b00

/*
 * The eight XIO IMPACT variants:
 *
 * - SI+T and ESI+T come from owner gfxinfo captures of real boards. Their
 *   revisions are established; 0x072488 bit 7, which IRIX never reads, is
 *   approximated as 0, and so is the whole 0x072408 byte beyond "path 0
 *   holds TRAM, path 1 does not" (0x0c).
 * - SI and ESI are inferred from those two boards with the TRAM taken
 *   away: nobody has captured gfxinfo from a textureless board. 0x072408
 *   is approximated as 0x0f, both TRAM paths absent.
 * - SSI, MXI, ESSI and EMXI have no capture at all. Their counts and
 *   names are established by the OCTANE Technical Report and IRIX's own
 *   decode; product ID 2 (over 3), the MGRAS revision and every ASIC
 *   revision are approximated with the matching one-GE board's values.
 *
 * Only 0x071308 is left out: its bits 7:4 are the attached monitor's ID,
 * not the board's.
 *
 * Every board scans out 1280x1024, the screen IRIX's default timing table
 * sets up. Which boards also drive 1600x1200 is unsettled: the OCTANE
 * Technical Report's feature list says only MXI does, while its Table 4.3
 * lists SI and SSI visuals at 1600x1200 from an IRIX 5.3 and 6.2 patch.
 * The model has no mode change, so every board keeps 1280x1024.
 */
static const SGIMgrasBoard mgras_boards[] = {
    {
        .name = "si", .hinv_name = "SI",
        .width = 1280, .height = 1024,
        .ge_count = 1, .re_count = 1, .tram_count = 0,
        .version1 = 0x45, .version0 = 0x0f,
        .hq_revision = 2, .ge_revision = 2, .re4_revision = 2,
        .pp1_revision = 0, .vc3_revision = 0, .cmap_revision = 3,
        .ra_revision = 0, .video_option = false,
        .implemented = true,
    }, {
        .name = "si-t", .hinv_name = "SI with texture option",
        .width = 1280, .height = 1024,
        .ge_count = 1, .re_count = 1, .tram_count = 4,
        .version1 = 0x45, .version0 = 0x0c,
        .hq_revision = 2, .ge_revision = 2, .re4_revision = 2,
        .pp1_revision = 0, .vc3_revision = 0, .cmap_revision = 3,
        .ra_revision = 0, .video_option = false,
    }, {
        .name = "ssi", .hinv_name = "SSI",
        .width = 1280, .height = 1024,
        .ge_count = 2, .re_count = 2, .tram_count = 0,
        .version1 = 0x46, .version0 = 0x0f,
        .hq_revision = 2, .ge_revision = 2, .re4_revision = 2,
        .pp1_revision = 0, .vc3_revision = 0, .cmap_revision = 3,
        .ra_revision = 0, .video_option = false,
    }, {
        .name = "mxi", .hinv_name = "MXI",
        .width = 1280, .height = 1024,
        .ge_count = 2, .re_count = 2, .tram_count = 4,
        .version1 = 0x46, .version0 = 0x00,
        .hq_revision = 2, .ge_revision = 2, .re4_revision = 2,
        .pp1_revision = 0, .vc3_revision = 0, .cmap_revision = 3,
        .ra_revision = 0, .video_option = false,
    }, {
        /*
         * Approximated beyond identity: the model renders an E-series
         * board exactly as its first-generation counterpart. The GE is
         * never executed, and no capture shows the later ASICs (GE12,
         * PP1 revision E, MGRAS revision 4) behaving differently to
         * software. A guest that relies on a GE12 or PP1 E difference
         * will not see it.
         */
        .name = "esi", .hinv_name = "ESI",
        .width = 1280, .height = 1024,
        .ge_count = 1, .re_count = 1, .tram_count = 0,
        .version1 = 0x51, .version0 = 0x0f,
        .hq_revision = 2, .ge_revision = 3, .re4_revision = 2,
        .pp1_revision = 4, .vc3_revision = 0, .cmap_revision = 3,
        .ra_revision = 0, .video_option = false,
        .implemented = true,
    }, {
        .name = "esi-t", .hinv_name = "ESI with texture option",
        .width = 1280, .height = 1024,
        .ge_count = 1, .re_count = 1, .tram_count = 4,
        .version1 = 0x51, .version0 = 0x0c,
        .hq_revision = 2, .ge_revision = 3, .re4_revision = 2,
        .pp1_revision = 4, .vc3_revision = 0, .cmap_revision = 3,
        .ra_revision = 0, .video_option = false,
    }, {
        .name = "essi", .hinv_name = "ESSI",
        .width = 1280, .height = 1024,
        .ge_count = 2, .re_count = 2, .tram_count = 0,
        .version1 = 0x52, .version0 = 0x0f,
        .hq_revision = 2, .ge_revision = 3, .re4_revision = 2,
        .pp1_revision = 4, .vc3_revision = 0, .cmap_revision = 3,
        .ra_revision = 0, .video_option = false,
    }, {
        .name = "emxi", .hinv_name = "EMXI",
        .width = 1280, .height = 1024,
        .ge_count = 2, .re_count = 2, .tram_count = 4,
        .version1 = 0x52, .version0 = 0x00,
        .hq_revision = 2, .ge_revision = 3, .re4_revision = 2,
        .pp1_revision = 4, .vc3_revision = 0, .cmap_revision = 3,
        .ra_revision = 0, .video_option = false,
    },
};

static const SGIMgrasBoard *mgras_find_board(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(mgras_boards); i++) {
        if (g_str_equal(mgras_boards[i].name, name)) {
            return &mgras_boards[i];
        }
    }
    return NULL;
}

/* 0x071708: RA revision 6:4, CMAP revision 2:0, bit 7 set: no video option. */
static uint8_t mgras_cmap_ra_byte(const SGIMgrasBoard *b)
{
    return (b->video_option ? 0 : 0x80) | (b->ra_revision & 7) << 4 |
           (b->cmap_revision & 7);
}

/*
 * An interrupt packet into the widget-addressed fabric, through the XIO
 * widget layer (hw/mips/sgi/xio-widget.h).
 */
static void mgras_send_packet(SGIMgrasState *s, hwaddr where, uint8_t vector,
                           bool set)
{
    /* Retain the packet for host-only qtest inspection. */
    s->test_last_interrupt_addr = where;
    s->test_last_interrupt_value = vector | (set ? SGI_XIO_INTERRUPT_SET : 0);
    sgi_xio_send_interrupt(&s->fabric_as, where, vector, set);
}

/* The fabric address of a widget/address pair: the widget in bits 51:48. */
bool sgi_mgras_xio_compose(SGIMgrasState *s, unsigned int widget,
                      uint64_t address, hwaddr *fabric)
{
    return sgi_xio_compose("sgi-mgras", widget, address, fabric);
}

/*
 * Widget errors use the generic destination pair (0x30/0x38, with the
 * aliases at 0x34/0x3c) and its embedded vector.
 */
static void mgras_deliver_widget_error(SGIMgrasState *s, bool set)
{
    SGIXioIntDest dest = sgi_xio_int_dest(s->int_dest_upper,
                                          s->int_dest_lower);
    hwaddr where;

    if (!s->int_dest_upper && !s->int_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: widget-error interrupt %s "
                      "with no destination programmed; no packet sent\n",
                      set ? "raised" : "lowered");
        return;
    }
    if (!sgi_mgras_xio_compose(s, dest.widget, dest.address, &where)) {
        return;
    }
    mgras_send_packet(s, where, dest.vector, set);
}

/* HQ roles share one destination and select separate stored vectors. */
static void mgras_deliver_hq_role(SGIMgrasState *s, bool set, uint8_t vector,
                               const char *role)
{
    unsigned int widget = (s->hq_vector_dest_upper >>
                           MGRAS_HQ_VECTOR_WIDGET_SHIFT) &
                          ((1u << MGRAS_HQ_VECTOR_WIDGET_BITS) - 1);
    uint64_t addr = ((uint64_t)(s->hq_vector_dest_upper & 0xffff) << 32) |
                    s->hq_vector_dest_lower;
    hwaddr where;

    if (!s->hq_vector_dest_upper && !s->hq_vector_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: %s interrupt %s with no "
                      "HQ vector destination programmed; no packet sent\n",
                      role, set ? "raised" : "lowered");
        return;
    }
    if (!sgi_mgras_xio_compose(s, widget, addr, &where)) {
        return;
    }
    mgras_send_packet(s, where, vector, set);
}

static void mgras_update_irq(SGIMgrasState *s)
{
    /* The low-water top-level mask bit is unknown and remains unsupported. */
    bool lowater = !!(s->pending & MGRAS_PENDING_LOWATER);
    bool general = !!(s->pending & s->irq_mask &
                      (MGRAS_PENDING_FLAGS | MGRAS_PENDING_CONTEXT |
                       SGI_MGRAS_PENDING_SWAP));
    bool error = !!(s->pending & s->irq_mask & MGRAS_PENDING_ERROR);

    uint8_t lowater_vector = s->hq_vectors >> MGRAS_HQ_VECTOR_LOWATER_SHIFT;
    uint8_t general_vector = s->hq_vectors >> MGRAS_HQ_VECTOR_GENERAL_SHIFT;

    /* This approximation sends one packet for each modeled transition. */
    if (lowater != s->lowater_asserted) {
        s->lowater_asserted = lowater;
        mgras_deliver_hq_role(s, lowater, lowater_vector, "low-water");
    }
    if (general != s->general_asserted) {
        s->general_asserted = general;
        mgras_deliver_hq_role(s, general, general_vector, "general");
    }
    if (error != s->error_asserted) {
        s->error_asserted = error;
        mgras_deliver_widget_error(s, error);
    }
}

void sgi_mgras_raise_pending(SGIMgrasState *s, uint32_t sources)
{
    s->pending |= sources;
    mgras_update_irq(s);
}

static void mgras_error(SGIMgrasState *s, hwaddr addr, unsigned size,
                     const char *why)
{
    s->error = 1;
    s->error_addr = addr;
    s->error_width = size;
    s->protocol = 0xff;
    s->pending |= MGRAS_PENDING_ERROR;
    mgras_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: %s at 0x%" HWADDR_PRIx
                  " width %u\n", why, addr, size);
}

/*
 * The kernel's retrace handler acknowledges nothing on the board, and the
 * 0x011040 enable word it programs leaves bit 0 clear, so a retrace is a
 * message sent at every retrace while VC3 enables it, not a level held
 * until an acknowledge. The Hub's pending bit, which the kernel clears
 * itself, is the only latch. Retrace period and phase are approximate.
 */
static void mgras_retrace(void *opaque)
{
    SGIMgrasState *s = opaque;
    if (s->display_enabled) {
        bool vintr = s->vc3_reg[MGRAS_VC3_CONTROL] & MGRAS_VC3_CONTROL_VINTR;
        bool dest = s->hq_vector_dest_upper || s->hq_vector_dest_lower;

        s->retraces++;
        s->pending |= MGRAS_PENDING_RETRACE;
        if (vintr && dest) {
            mgras_deliver_hq_role(s, true, s->hq_vectors >>
                                  MGRAS_HQ_VECTOR_RETRACE_SHIFT, "retrace");
        } else if (vintr && !s->retrace_no_dest_logged) {
            /* Once per reset: a retrace comes 60 times a second. */
            s->retrace_no_dest_logged = true;
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: retrace interrupt "
                          "enabled with no HQ vector destination programmed; "
                          "no packets sent\n");
        }
    }
    timer_mod(s->retrace_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void mgras_complete_context(SGIMgrasState *s)
{
    sgi_mgras_gl_context_loaded(s);
    s->ready = true;
    s->protocol = 3;
    s->pending |= MGRAS_PENDING_CONTEXT;
    mgras_update_irq(s);
}

static void mgras_context_write(SGIMgrasState *s, uint32_t value)
{
    if (!s->entry_selected || s->protocol != 1) {
        mgras_error(s, 0x120400, 4, "context write in invalid state");
        return;
    }
    if (!s->first_ack) {
        mgras_error(s, 0x120400, 4,
                    "context write before first acknowledgement");
        return;
    }
    if (s->context_words >= 63) {
        /*
         * Established (mgras-hq4.md, "Context switch and HQ flags"):
         * MgrasCtxsw stores exactly 63 words and never reads or writes a
         * closing token at 0x120400 -- the transaction completes as soon
         * as the 63rd word lands. Defensive only: completion moves
         * protocol off the image state, so mgras_write() already routes any
         * later word to the command parser and this branch is unreachable
         * through it.
         */
        mgras_error(s, 0x120400, 4, "context write after image is complete");
        return;
    }
    s->context[s->context_words++] = value;
    if (s->context_words == 63) {
        mgras_complete_context(s);
    }
}

/*
 * This approximation stores uploaded GE words without interpreting them.
 * Other control values end the upload, and bit 31 rewinds its read pointer.
 */
static void mgras_ge_control_write(SGIMgrasState *s, unsigned int slot,
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
static void mgras_ge_data_write(SGIMgrasState *s, hwaddr addr, unsigned size,
                             unsigned int slot, uint32_t value)
{
    uint32_t ptr;

    if (!s->ge_load_mode[slot]) {
        return;
    }
    ptr = s->ge_write_ptr[slot];
    if (ptr >= SGI_MGRAS_GE_WORDS) {
        mgras_error(s, addr, size, "GE upload overflow");
        return;
    }
    s->ge[slot][ptr] = value;
    s->ge_write_ptr[slot] = ptr + 1;
    s->ge_words[slot] = MAX(s->ge_words[slot], ptr + 1);
}

/*
 * The selector carries the color map in bits 4:0 and the first entry in
 * bits 15:8; each data-port access takes the next entry and wraps inside
 * the 256-entry map. Bits 7:5 are zero in every selector the stock X
 * server writes. Established from the retained desktop capture.
 */
static uint32_t mgras_cmap_address(SGIMgrasState *s)
{
    return (s->cmap_selector & 0x1f) * 256 +
           (((s->cmap_selector >> 8) + s->cmap_position) & 0xff);
}

/*
 * DAC registers are addressed by the halfword at SGI_MGRAS_DAC_LUT_INDEX,
 * register number in the high byte, and read and written a byte at a time
 * through SGI_MGRAS_DAC_LUT_DATA_2. SGI's IDE DAC reset accepts ID 0x79
 * with revision 1, or ID 0x76 with revision 0 after a warning; the model
 * answers the pair the diagnostic takes without warning. The part and its
 * real values are unidentified. Every other address is plain storage that
 * reads back what was written, which is an approximation: a real DAC has
 * read-only and unimplemented registers the model does not distinguish.
 */
static uint8_t mgras_dac_reg_read(SGIMgrasState *s)
{
    uint16_t addr = s->dac_lut_index;

    switch (addr) {
    case MGRAS_DAC_REG_ID:
        return 0x79;
    case MGRAS_DAC_REG_REVISION:
        return 0x01;
    default:
        return s->dac_reg[addr];
    }
}

static uint64_t mgras_read_internal(SGIMgrasState *s, hwaddr addr,
                                     unsigned size)
{
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        mgras_error(s, addr, size, "unsupported read width");
        return 0;
    }
    if ((addr == 0x071c88 && size != 1) ||
        (addr == SGI_MGRAS_XMAP_MAIN_MODE && size != 4) ||
        (addr == SGI_MGRAS_XMAP_BUF_SELECT && size != 4) ||
        (addr == 0x070c30 && size != 2) ||
        (addr == 0x070d18 && size != 4)) {
        mgras_error(s, addr, size, "unsupported register read width");
        return 0;
    }
    if (size == 1 && addr != SGI_MGRAS_I2C_SCRATCH && addr != 0x071208 &&
        addr != 0x071c88 && addr != 0x071308 && addr != SGI_MGRAS_I2C_STROBE &&
        addr != 0x071708 && addr != SGI_MGRAS_BOARD_INVENTORY &&
        addr != SGI_MGRAS_DAC_LUT_DATA_2) {
        mgras_error(s, addr, size, "unsupported byte read");
        return 0;
    }
    if (size == 2 && addr != SGI_MGRAS_VC3_DATA &&
        addr != SGI_MGRAS_VC3_RAM && addr != SGI_MGRAS_DAC_LUT_INDEX) {
        mgras_error(s, addr, size, "unsupported halfword read");
        return 0;
    }
    switch (addr) {
    /*
     * Inferred: reserved halves of the 64-bit-aligned widget-register
     * slots (the real registers use only the second word, at 0x000004
     * and 0x000024). No public source documents guest behavior at these
     * offsets; treat them as read-zero rather than raising a guest error
     * a real access hits.
     */
    case 0x000020:
        return 0;
    /*
     * Bits 31:28 are the HQ revision the IRIX kernel reads with a 32-bit
     * load (mgras-board-identity.md). How they relate to the widget
     * identification word at 0x000004 is unknown; a 64-bit read keeps
     * the low word at zero as before.
     */
    case 0x000000: {
        uint32_t v = (uint32_t)(s->board->hq_revision & 0xf) << 28;

        return size == 8 ? (uint64_t)v << 32 : v;
    }
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
    /*
     * The board-version bytes, each the top byte of its word: TRAM
     * presence per raster path here, product ID, MGRAS revision and GE
     * count at SGI_MGRAS_BOARD_INVENTORY. On this big-endian bus a byte
     * read returns that top byte. Writes do not change them.
     */
    case SGI_MGRAS_RESET_CONTROL:
        return (uint32_t)s->board->version0 << 24;
    case SGI_MGRAS_BOARD_INVENTORY:
        return size == 1 ? s->board->version1 :
               (uint32_t)s->board->version1 << 24;
    case 0x071708: {
        uint8_t v = mgras_cmap_ra_byte(s->board);

        return size == 1 ? v : (uint32_t)v << 24;
    }
    /*
     * IRIX reads this just before 0x028004 and discards it. What hardware
     * returns is unobserved; zero is an approximation, kept distinct from
     * the GE revision so a guest that took its revision from here would
     * see the difference.
     */
    case 0x028000:
        return 0;
    /*
     * After the GE program verifies, the kernel takes the GE revision from
     * the low byte here; 3 or more marks an E-series board. Every class-0
     * 0xa2 query, whether the kernel's own (MgrasFinish, index 0x1fa) or
     * the OpenGL client's, is answered here once HQ flag bit 17 is set
     * (see mgras_raster_command_fifo_dispatch()). Approximated: the model
     * holds the GE revision from reset until the first query of either
     * kind, then answers zero. When real hardware replaces the revision,
     * and what each query returns, are unknown.
     */
    case 0x028004:
        return s->ge_report;
    /*
     * The kernel's GFX_GETBOARDINFO handler performs a one-byte read here
     * on every OpenGL context query; the captured flow answers 0 and
     * raises no error (glx-ioctl-trace.md). The field's meaning is
     * otherwise unknown.
     */
    case 0x071308:
        return 0;
    case SGI_MGRAS_BRINGUP_ARG_2: return s->bringup_arg_2;
    case SGI_MGRAS_BRINGUP_BLOCK_1: return s->bringup_block[0];
    case SGI_MGRAS_BRINGUP_BLOCK_2: return s->bringup_block[1];
    case SGI_MGRAS_BRINGUP_BLOCK_3: return s->bringup_block[2];
    case SGI_MGRAS_SCRATCH_COMMAND: return s->scratch_command;
    case SGI_MGRAS_BRINGUP_ARG_3: return s->bringup_arg_3;
    case SGI_MGRAS_VC3_ARG: return s->vc3_arg;
    case SGI_MGRAS_XMAP_OVERLAY_MODE:
        return s->xmap_overlay_mode[s->xmap_index];
    /* XMAP selection and per-visual modes are an approximation. */
    case SGI_MGRAS_XMAP_MAIN_MODE: return s->xmap_mode[s->xmap_index];
    case SGI_MGRAS_XMAP_BUF_SELECT: return s->xmap_buf_select[s->xmap_index];
    case SGI_MGRAS_I2C_SCRATCH: return s->i2c_scratch;
    /*
     * Byte reads mirror the write side's no-op treatment (case
     * SGI_MGRAS_I2C_STROBE below): approximated fixed idle/ack value. No
     * public source documents this port's read-back semantics.
     */
    case SGI_MGRAS_I2C_STROBE: return 0;
    case SGI_MGRAS_REPP_STATUS_1: return s->repp_status[0];
    /*
     * SGI's IDE XMAP test reads the XMAP revision under index byte 4 and
     * accepts 0 or 1, and the FIFO capacity under index byte 8 and
     * requires a low nibble of 1. Which revision the part reports is
     * unknown; zero is the model's usual answer for an unknown revision.
     */
    case SGI_MGRAS_REPP_STATUS_3:
        if (s->xmap_index == MGRAS_XMAP_CONFIG_REVISION) {
            return s->board->pp1_revision;
        }
        if (s->xmap_index == MGRAS_XMAP_CONFIG_FIFO_CAPACITY) {
            return 1;
        }
        return s->repp_status[1];
    case SGI_MGRAS_REPP_STATUS_4: return s->repp_status[2];
    case SGI_MGRAS_SYNC_STATUS: return s->sync_status;
    case SGI_MGRAS_BRINGUP_FLAG: return s->bringup_flag;
    case SGI_MGRAS_DAC_LUT_INDEX: return s->dac_lut_index;
    case SGI_MGRAS_DAC_LUT_DATA: return s->dac_lut_data[0];
    case SGI_MGRAS_DAC_LUT_DATA_2: return mgras_dac_reg_read(s);
    case SGI_MGRAS_DAC_CONFIG: return s->dac_config;
    case SGI_MGRAS_DAC_LUT_INDEX_LOW: return s->dac_lut_index_low;
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
    /* Bits 7:4 are the RE4 revision; the rest is approximated. */
    case 0x02c578: return 0x100 | (s->board->re4_revision & 0xf) << 4;
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

        /* Register 7 is the SRAM address; it reads back as loaded. */
        if (s->vc3_index == MGRAS_VC3_RAM_ADDR) {
            v = s->vc3_ram_addr;
        }
        /*
         * Register 0x1f carries the VC3 revision in bits 7:6, as both the
         * IRIX kernel and SGI's IDE read it; the other bits echo storage.
         */
        if (s->vc3_index == 0x1f) {
            v = (v & ~0xc0) | (s->board->vc3_revision & 3) << 6;
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
    case 0x070d18: return s->cmap[mgras_cmap_address(s)];
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
        mgras_error(s, addr, size, "unsupported read");
        return 0;
    }
}

static uint64_t mgras_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIMgrasState *s = opaque;
    uint64_t value = mgras_read_internal(s, addr, size);
    uint64_t sequence = s->trace_sequence++;

    trace_sgi_mgras_mmio(sequence, addr, value, size, false);
    if (addr != MGRAS_CONTEXT_COMMAND && addr != SGI_MGRAS_SCRATCH_COMMAND) {
        trace_sgi_mgras_control_mmio(sequence, addr, value, size, false);
    }
    if (addr >= SGI_MGRAS_GE_PORTS && addr < SGI_MGRAS_GE_PORTS_END) {
        sgi_mgras_gl_ge_port_access(s, addr, false);
    }
    return value;
}

static void mgras_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    SGIMgrasState *s = opaque;
    uint64_t sequence = s->trace_sequence++;

    trace_sgi_mgras_mmio(sequence, addr, value, size, true);
    if (addr != MGRAS_CONTEXT_COMMAND && addr != SGI_MGRAS_SCRATCH_COMMAND) {
        trace_sgi_mgras_control_mmio(sequence, addr, value, size, true);
    }
    if (addr >= SGI_MGRAS_GE_PORTS && addr < SGI_MGRAS_GE_PORTS_END) {
        sgi_mgras_gl_ge_port_access(s, addr, true);
    }
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        (addr & (size - 1))) {
        mgras_error(s, addr, size, "unsupported or unaligned write");
        return;
    }
    if ((addr == 0x071c88 && size != 1) ||
        (addr == SGI_MGRAS_XMAP_MAIN_MODE && size != 4) ||
        (addr == SGI_MGRAS_XMAP_BUF_SELECT && size != 4) ||
        (addr == 0x070c30 && size != 2) ||
        (addr == 0x070d18 && size != 4)) {
        mgras_error(s, addr, size, "unsupported register write width");
        return;
    }
    /*
     * A captured desktop session writes the gamma-table load registers at
     * these widths: byte for DAC_LUT_INDEX_LOW and DAC_LUT_DATA_2, and
     * halfword for DAC_LUT_INDEX and DAC_LUT_DATA. See the header comment
     * above SGI_MGRAS_DAC_LUT_INDEX.
     */
    if (size == 1 && addr != SGI_MGRAS_I2C_SCRATCH &&
        addr != SGI_MGRAS_I2C_STROBE && addr != SGI_MGRAS_VC3_INDEX &&
        addr != 0x071c88 && addr != SGI_MGRAS_DAC_LUT_INDEX_LOW &&
        addr != SGI_MGRAS_DAC_LUT_DATA_2 && addr != SGI_MGRAS_DAC_CONFIG &&
        addr != SGI_MGRAS_REPP_STATUS_1 && addr != SGI_MGRAS_REPP_STATUS_4 &&
        addr != SGI_MGRAS_VC3_ARG) {
        mgras_error(s, addr, size, "unsupported byte write");
        return;
    }
    if (size == 2 && addr != 0x070c30 && addr != SGI_MGRAS_VC3_RAM &&
        addr != SGI_MGRAS_DAC_LUT_INDEX && addr != SGI_MGRAS_DAC_LUT_DATA) {
        mgras_error(s, addr, size, "unsupported halfword write");
        return;
    }
    switch (addr) {
    /*
     * Inferred: reserved halves of the 64-bit-aligned widget-register
     * slots (the real registers use only the second word, at 0x000004
     * and 0x000024). No public source documents guest behavior at these
     * offsets; ignore writes rather than raising a guest error a real
     * access hits.
     */
    case 0x000000:
    case 0x000020:
        return;
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
    case SGI_MGRAS_SCRATCH_COMMAND:
        s->scratch_command = value;
        sgi_mgras_dma_map_write(s, value, size);
        return;
    /* Clear modeled low-water state when its enable is cleared. */
    case SGI_MGRAS_BRINGUP_ARG_3:
        s->bringup_arg_3 = value;
        if ((value & s->hq_enable) & MGRAS_HQ_ENABLE_LOWATER) {
            s->pending &= ~MGRAS_PENDING_LOWATER;
        }
        s->hq_enable &= ~(uint32_t)value;
        mgras_update_irq(s);
        return;
    case SGI_MGRAS_VC3_ARG: s->vc3_arg = value; return;
    case SGI_MGRAS_XMAP_OVERLAY_MODE:
        s->xmap_overlay_mode[s->xmap_index] = value;
        s->dirty = true;
        return;
    /* Per-visual mode storage and scanout interpretation are approximate. */
    case SGI_MGRAS_XMAP_MAIN_MODE:
        s->xmap_mode[s->xmap_index] = value;
        s->dirty = true;
        return;
    /* Swap-mask storage; see the header comment above the definition. */
    case SGI_MGRAS_XMAP_BUF_SELECT:
        s->xmap_buf_select[s->xmap_index] = value;
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
    /*
     * A write resets the gamma-table load position to row 0, channel 0,
     * regardless of value. Only the final write before each load's row
     * writes is confirmed to always be zero; two of the three captured
     * loads precede that with seven more writes of other values (see
     * the header comment above SGI_MGRAS_DAC_LUT_INDEX). The value
     * itself is only echoed back, not otherwise interpreted.
     */
    case SGI_MGRAS_DAC_LUT_INDEX:
        s->dac_lut_index = value;
        s->dac_lut_load_pos = 0;
        s->dac_lut_load_channel = 0;
        return;
    /*
     * Each write's high byte loads the current row/channel of the gamma
     * table, then advances: red, then green, then blue, then the next
     * row. See the header comment above SGI_MGRAS_DAC_LUT_INDEX.
     */
    case SGI_MGRAS_DAC_LUT_DATA:
        s->dac_lut_data[0] = value;
        s->dac_gamma[s->dac_lut_load_channel][s->dac_lut_load_pos] = value >> 8;
        if (++s->dac_lut_load_channel == 3) {
            s->dac_lut_load_channel = 0;
            s->dac_lut_load_pos++;
        }
        s->dirty = true;
        return;
    case SGI_MGRAS_DAC_LUT_DATA_2:
        s->dac_lut_data[1] = value;
        s->dac_reg[s->dac_lut_index & 0xffff] = value;
        return;
    case SGI_MGRAS_DAC_CONFIG: s->dac_config = value; return;
    /*
     * A byte here lands in the register-number byte of the DAC address,
     * bits 15:8 of the halfword at SGI_MGRAS_DAC_LUT_INDEX: SGI's IDE
     * address test writes this byte and requires it back there.
     */
    case SGI_MGRAS_DAC_LUT_INDEX_LOW:
        s->dac_lut_index_low = value;
        s->dac_lut_index = (s->dac_lut_index & 0xffu) |
                           ((uint32_t)(value & 0xff) << 8);
        return;
    case 0x011000:
        /* Retain the unresolved prefix and use only its presence. */
        s->prefix = value;
        s->prefix_written = true;
        return;
    case 0x011028:
        s->pending &= ~(uint32_t)value;
        if (value & MGRAS_PENDING_FLAGS) s->first_ack = true;
        if (value & MGRAS_PENDING_CONTEXT) s->second_ack = true;
        mgras_update_irq(s); return;
    /* Store unresolved status bits separately from pending sources. */
    case 0x011020: s->hq_status_extra = value; return;
    /*
     * Inferred: 0x011040 sets enables and 0x011048 clears them. The IRIX
     * 6.5.30 kernel stores 0x400 to 0x011040 before a swap, and nothing
     * after that writes 0x011040 again before the next MgrasCtxsw sleeps on
     * source 19; the switch's tail clears every enable at 0x011048 before
     * restoring the incoming context's word. With a replacing store the
     * source-19 wake never arrives, and the switch is left to its watchdog.
     * The discriminating tests are
     * irq-enable-set-and-clear-ports and swap-keeps-context-switch-enable.
     */
    case 0x011040:
        s->irq_mask |= value;
        mgras_update_irq(s);
        return;
    case 0x011048:
        s->irq_mask &= ~(uint32_t)value;
        s->error = 0;
        s->pending &= ~MGRAS_PENDING_ERROR;
        mgras_update_irq(s);
        return;
    case SGI_MGRAS_HQ_CONFIG_A: s->hq_config_a = value; return;
    case SGI_MGRAS_HQ_CONFIG_B: s->hq_config_b = value; return;
    case SGI_MGRAS_HQ_ENABLE:
        /* Immediate readiness raises the modeled low-water source. */
        if ((value & ~s->hq_enable) & MGRAS_HQ_ENABLE_LOWATER) {
            s->pending |= MGRAS_PENDING_LOWATER;
        }
        s->hq_enable = value;
        mgras_update_irq(s);
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
            mgras_error(s, addr, size, "invalid HQ entry"); return;
        }
        s->entry = value; s->entry_selected = true; s->protocol = 1;
        s->first_ack = s->second_ack = false;
        /* A new transaction restarts the context word count. */
        s->context_words = 0;
        s->pending |= MGRAS_PENDING_FLAGS; mgras_update_irq(s); return;
    case 0x120400:
        /*
         * s->ready alone cannot gate this dispatch: it latches true after
         * the first context switch and stays true, but a later switch's
         * image words (protocol == 1, mid-transaction) must still reach
         * mgras_context_write() even though the port has been ready since an
         * earlier switch. Only route to the command-FIFO parsers once a
         * transaction is not in its image-upload phase.
         */
        if (size == 4 && s->ready && s->protocol != 1) {
            sgi_mgras_raster_command_token(s, value);
        } else if (size == 4) {
            mgras_context_write(s, value);
        } else if (size == 8 && s->ready && s->protocol != 1) {
            sgi_mgras_raster_command(s, value);
        } else if (size == 8 && !s->entry_selected) {
            /* Pre-selection traffic is opaque and has no modeled effect. */
            return;
        } else {
            mgras_error(s, addr, size, "unsupported context width");
        }
        return;
    case 0x070c30:
        s->cmap_selector = value;
        s->cmap_position = 0;
        return;
    case 0x070d18:
        s->cmap[mgras_cmap_address(s)] = value;
        /* The position is an 8-bit count a selector write resets. */
        s->cmap_position++;
        s->dirty = true;
        return;
    case 0x071f00: return;
    /* The unresolved synchronization selector is accepted but not stored. */
    case SGI_MGRAS_REPP_SYNC: return;
    /* Index 7 selects SRAM. Other indices update the stored VC3 state. */
    case SGI_MGRAS_VC3_INDEXDATA: {
        uint8_t index = value >> 24;
        uint16_t data16 = (value >> 8) & 0xffff;

        if (index == MGRAS_VC3_RAM_ADDR) {
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
        /* The display-ID table pointers; see sgi-mgras-display.c. */
        case 0x04:
        case 0x05:
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
        case MGRAS_VC3_CONTROL: {
            /* Redraw only when display enable changes. */
            bool enabled = (data16 & MGRAS_VC3_CONTROL_DISPLAY) != 0;
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
        mgras_ge_control_write(s, SGI_MGRAS_GE_SLOT_A, value);
        return;
    }
    if (addr == SGI_MGRAS_GE_CTRL_B) {
        mgras_ge_control_write(s, SGI_MGRAS_GE_SLOT_B, value);
        return;
    }
    if (addr == SGI_MGRAS_GE_DATA_A) {
        mgras_ge_data_write(s, addr, size, SGI_MGRAS_GE_SLOT_A, value);
        return;
    }
    if (addr == SGI_MGRAS_GE_DATA_B) {
        mgras_ge_data_write(s, addr, size, SGI_MGRAS_GE_SLOT_B, value);
        return;
    }
    mgras_error(s, addr, size, "unsupported write");
}

static const MemoryRegionOps mgras_ops = {
    .read = mgras_read, .write = mgras_write, .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    /* Preserve 64-bit FIFO words instead of splitting them into callbacks. */
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void mgras_reset_enter(Object *obj, ResetType type)
{
    SGIMgrasClass *mc = SGI_MGRAS_GET_CLASS(obj);
    SGIMgrasState *s = SGI_MGRAS(obj);

    if (mc->parent_phases.enter) {
        mc->parent_phases.enter(obj, type);
    }
    memset(s->front, 0, s->width * s->height * sizeof(*s->front));
    memset(s->back, 0, s->width * s->height * sizeof(*s->back));
    memset(s->depth, 0, s->width * s->height * sizeof(*s->depth));
    memset(s->format, 0, s->width * s->height);
    memset(s->overlay, 0, s->width * s->height);
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
    s->glyph_row = s->glyph_unit = s->pio_x = s->pio_y = 0;
    s->cmd_token_class = s->cmd_token_reg = 0;
    s->cmd_token_words_remaining = s->cmd_token_payload_count = 0;
    memset(s->cmd_token_payload, 0, sizeof(s->cmd_token_payload));
    sgi_mgras_gl_reset(s);
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
        s->xmap_buf_select[i] = 0;
    }
    memset(s->xmap_overlay_mode, 0, sizeof(s->xmap_overlay_mode));
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
    s->dac_lut_index_low = 0;
    s->dac_lut_load_pos = 0;
    s->dac_lut_load_channel = 0;
    memset(s->dac_reg, 0, sizeof(s->dac_reg));
    for (int i = 0; i < 256; i++) {
        s->dac_gamma[0][i] = s->dac_gamma[1][i] = s->dac_gamma[2][i] = i;
    }
    memset(s->config_table, 0, sizeof(s->config_table));
    s->bringup_flag = 0;
    s->bringup_arg_2 = 0;
    memset(s->bringup_block, 0, sizeof(s->bringup_block));
    s->scratch_command = 0;
    memset(s->dma_map_regs, 0, sizeof(s->dma_map_regs));
    s->dma_map_reg = s->dma_map_words_remaining = 0;
    memset(s->dma_desc_20000, 0, sizeof(s->dma_desc_20000));
    memset(s->dma_desc_28000, 0, sizeof(s->dma_desc_28000));
    memset(s->dma_desc_30000, 0, sizeof(s->dma_desc_30000));
    s->bringup_arg_3 = 0;
    s->vc3_arg = 0;
    s->hq_status_extra = 0;
    s->hq_vector_dest_upper = s->hq_vector_dest_lower = s->hq_vectors = 0;
    s->hq_config_a = s->hq_config_b = s->hq_enable = 0;
    memset(s->fifo_threshold, 0, sizeof(s->fifo_threshold));
    s->sched_arg_1 = s->sched_arg_2 = s->sched_arg_3 = s->sched_arg_4 = 0;
    s->hq_flags = 0;
    s->ge_report = s->board->ge_revision;
    s->lowater_asserted = s->general_asserted = s->error_asserted = false;
    s->retrace_no_dest_logged = false;
}

static void mgras_reset_hold(Object *obj, ResetType type)
{
    SGIMgrasClass *mc = SGI_MGRAS_GET_CLASS(obj);
    SGIMgrasState *s = SGI_MGRAS(obj);

    if (mc->parent_phases.hold) {
        mc->parent_phases.hold(obj, type);
    }
    mgras_update_irq(s);
}

static void mgras_reset_exit(Object *obj, ResetType type)
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

/*
 * Select the descriptor. A board with TRAM or a second GE and RE would
 * make IRIX drive texture and multi-engine paths the model does not have,
 * so it is refused unless x-unimplemented-board asks for it anyway; then
 * it is presented with a LOG_UNIMP note naming what is missing.
 */
static bool mgras_realize_board(SGIMgrasState *s, Error **errp)
{
    const char *name = s->board_name ? s->board_name : "si";
    const SGIMgrasBoard *b = mgras_find_board(name);
    g_autoptr(GString) missing = NULL;

    if (!b) {
        g_autoptr(GString) names = g_string_new(NULL);

        for (size_t i = 0; i < ARRAY_SIZE(mgras_boards); i++) {
            g_string_append_printf(names, "%s%s", i ? ", " : "",
                                   mgras_boards[i].name);
        }
        error_setg(errp, "sgi-mgras: unknown board '%s'", name);
        error_append_hint(errp, "Valid boards: %s\n", names->str);
        return false;
    }
    if (!b->implemented) {
        missing = g_string_new(NULL);
        if (b->tram_count) {
            g_string_append(missing, "texture (TE1 and TRAM)");
        }
        if (b->ge_count > 1 || b->re_count > 1) {
            g_string_append_printf(missing, "%sa second GE and RE",
                                   missing->len ? " and " : "");
        }
        if (!s->unimplemented_board) {
            error_setg(errp, "sgi-mgras: board '%s' (%s) needs %s, which "
                       "the model does not implement", b->name, b->hinv_name,
                       missing->str);
            error_append_hint(errp, "Implemented boards: si, esi\n");
            return false;
        }
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: presenting board '%s' (%s) "
                      "without %s; IRIX will drive paths the model ignores\n",
                      b->name, b->hinv_name, missing->str);
    }
    if (b->width > SGI_MGRAS_MAX_WIDTH) {
        error_setg(errp, "sgi-mgras: board '%s' names a %u-pixel-wide "
                   "screen, but scanout rows hold %d pixels", b->name,
                   b->width, SGI_MGRAS_MAX_WIDTH);
        return false;
    }
    s->board = b;
    s->width = b->width;
    s->height = b->height;
    return true;
}

/*
 * The board's widget: one MMIO region answers both a processor's access
 * and a packet routed to this widget. Built before the base class moves
 * the board under its module, so nothing here may depend on the board's
 * QOM path; the console, which does, comes in mgras_finish_realize().
 */
static bool mgras_build_widget(SGIXioBoardState *board, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(board);

    if (!s->fabric) {
        error_setg(errp, "sgi-mgras needs the fabric its interrupt packets "
                  "travel");
        return false;
    }
    if (!mgras_realize_board(s, errp)) {
        return false;
    }
    s->front = g_new0(uint32_t, s->width * s->height);
    s->back = g_new0(uint32_t, s->width * s->height);
    s->depth = g_new0(uint32_t, s->width * s->height);
    s->front_count = s->back_count = s->width * s->height;
    s->depth_count = s->width * s->height;
    s->format = g_new0(uint8_t, s->width * s->height);
    s->overlay = g_new0(uint8_t, s->width * s->height);
    s->ge[SGI_MGRAS_GE_SLOT_A] = g_new0(uint32_t, SGI_MGRAS_GE_WORDS);
    s->ge[SGI_MGRAS_GE_SLOT_B] = g_new0(uint32_t, SGI_MGRAS_GE_WORDS);
    s->display_buffer = g_new0(uint32_t, s->width * s->height);
    memory_region_init_io(&s->regs, OBJECT(s), &mgras_ops, s,
                           "sgi-mgras.regs", SGI_MGRAS_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->regs);
    address_space_init(&s->fabric_as, s->fabric, "sgi-mgras.fabric");
    s->retrace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mgras_retrace, s);
    board->cpu_face = &s->regs;
    board->packet_face = &s->regs;
    return true;
}

/* Runs once the board sits at its final QOM path, which its console names. */
static bool mgras_finish_realize(SGIXioBoardState *board, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(board);
    DeviceState *dev = DEVICE(board);

    s->con = qemu_graphic_console_create(dev, 0, &sgi_mgras_display_ops, s);
    device_cold_reset(dev);
    sgi_mgras_display_ensure_surface(s);
    return true;
}

static void mgras_unrealize(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    SGIMgrasClass *mc = SGI_MGRAS_GET_CLASS(dev);

    mc->parent_unrealize(dev);
    timer_del(s->retrace_timer); timer_free(s->retrace_timer);
    address_space_destroy(&s->fabric_as);
    qemu_graphic_console_close(s->con);
    g_free(s->front); g_free(s->back); g_free(s->depth); g_free(s->format);
    g_free(s->overlay);
    g_free(s->ge[SGI_MGRAS_GE_SLOT_A]); g_free(s->ge[SGI_MGRAS_GE_SLOT_B]);
    g_free(s->display_buffer);
}

/* Rebuild host-only display state from migrated VC3 state and redraw it. */
static int mgras_post_load(void *opaque, int version_id)
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
         SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS) ||
        (s->cmd_token_class != SGI_MGRAS_CMD_TOKEN_DISCARD &&
         s->cmd_token_words_remaining > ARRAY_SIZE(s->cmd_token_payload)) ||
        s->dma_map_words_remaining > SGI_MGRAS_CMD_TOKEN_MAX_DISCARD_WORDS ||
        s->pio_x > s->width || s->pio_y > s->height ||
        s->xmap_index >= SGI_MGRAS_XMAP_ENTRIES ||
        s->dac_lut_load_channel >= 3 ||
        !sgi_mgras_gl_post_load(s)) {
        return -EINVAL;
    }

    sgi_mgras_display_ensure_surface(s);
    s->display_enabled =
        (s->vc3_reg[MGRAS_VC3_CONTROL] & MGRAS_VC3_CONTROL_DISPLAY) != 0;
    s->cursor_enabled = (s->vc3_reg[0x1d] & 0x2) != 0;
    s->dirty = true;
    return 0;
}

static bool mgras_front_count_valid(void *opaque, int version_id)
{
    SGIMgrasState *s = opaque;

    return s->front_count == s->width * s->height;
}

static bool mgras_back_count_valid(void *opaque, int version_id)
{
    SGIMgrasState *s = opaque;

    return s->back_count == s->width * s->height;
}

/*
 * Version 11 (introduced by the XMAP swap-mask select commit) still carries
 * both the retired per-pixel WID plane and the unindexed overlay-mode word
 * on the wire: that commit only added xmap_buf_select, it did not touch
 * either field. The WID-to-overlay and overlay-mode-to-per-WID-array cutover
 * lands at version 12, so streams from 9, 10 and 11 all need the old layout.
 */
static bool mgras_stream_before_v12(void *opaque, int version_id)
{
    return version_id < 12;
}

static bool mgras_ge_lengths_valid(void *opaque, int version_id)
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
    .name = TYPE_SGI_MGRAS, .version_id = 18, .minimum_version_id = 9,
    .post_load = mgras_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(front_count, SGIMgrasState),
        VMSTATE_VALIDATE("front plane length is fixed",
                         mgras_front_count_valid),
        VMSTATE_VARRAY_UINT32(front, SGIMgrasState, front_count, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(back_count, SGIMgrasState),
        VMSTATE_VALIDATE("back plane length is fixed", mgras_back_count_valid),
        VMSTATE_VARRAY_UINT32(back, SGIMgrasState, back_count, 0,
                              vmstate_info_uint32, uint32_t),
        /*
         * Streams before version 12 carry the retired per-pixel WID plane,
         * sized for the only screen the model then had.
         */
        VMSTATE_UNUSED_BUFFER(mgras_stream_before_v12, 0,
                              1280 * 1024),
        /* The per-pixel format tag; see mgras_put_pixel(). */
        VMSTATE_VARRAY_UINT32(format, SGIMgrasState, front_count, 10,
                              vmstate_info_uint8, uint8_t),
        /* So does the overlay plane. */
        VMSTATE_VARRAY_UINT32(overlay, SGIMgrasState, front_count, 12,
                              vmstate_info_uint8, uint8_t),
        VMSTATE_UINT32_ARRAY(hq, SGIMgrasState, 5120),
        VMSTATE_UINT32_V(hq_words, SGIMgrasState, 2),
        VMSTATE_UINT32_V(hq_hash, SGIMgrasState, 2),
        VMSTATE_UINT32_ARRAY_V(ge_words, SGIMgrasState, 2, 2),
        VMSTATE_VALIDATE("GE upload lengths are in range",
                         mgras_ge_lengths_valid),
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
        VMSTATE_UINT32(dac_lut_index_low, SGIMgrasState),
        VMSTATE_UINT8_V(dac_lut_load_pos, SGIMgrasState, 9),
        VMSTATE_UINT8_V(dac_lut_load_channel, SGIMgrasState, 9),
        VMSTATE_UINT8_2DARRAY_V(dac_gamma, SGIMgrasState, 3, 256, 9),
        VMSTATE_UINT8_ARRAY_V(dac_reg, SGIMgrasState, SGI_MGRAS_DAC_REGS, 13),
        VMSTATE_UINT32_ARRAY(config_table, SGIMgrasState,
                             SGI_MGRAS_CONFIG_TABLE_WORDS),
        VMSTATE_UINT32(bringup_flag, SGIMgrasState),
        VMSTATE_UINT32(bringup_arg_2, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(bringup_block, SGIMgrasState, 3),
        VMSTATE_UINT64(scratch_command, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(dma_map_regs, SGIMgrasState,
                             SGI_MGRAS_DMA_MAP_REGS),
        VMSTATE_UINT32(dma_map_reg, SGIMgrasState),
        VMSTATE_UINT32(dma_map_words_remaining, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(dma_desc_20000, SGIMgrasState, 12),
        VMSTATE_UINT32_ARRAY(dma_desc_28000, SGIMgrasState, 7),
        VMSTATE_UINT32_ARRAY(dma_desc_30000, SGIMgrasState, 1),
        VMSTATE_UINT32(bringup_arg_3, SGIMgrasState),
        VMSTATE_UINT32(vc3_arg, SGIMgrasState),
        /* Before version 12 this was one unindexed overlay-mode word. */
        VMSTATE_UNUSED_BUFFER(mgras_stream_before_v12, 0, 4),
        VMSTATE_UINT32_ARRAY_V(xmap_overlay_mode, SGIMgrasState,
                               SGI_MGRAS_XMAP_ENTRIES, 12),
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
        /* The retired retrace line state; retrace is now a message. */
        VMSTATE_UNUSED(1),
        VMSTATE_BOOL(error_asserted, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(rss, SGIMgrasState, 0x200),
        VMSTATE_UINT32(glyph_row, SGIMgrasState),
        VMSTATE_UINT32(glyph_unit, SGIMgrasState),
        VMSTATE_UINT32(pio_x, SGIMgrasState),
        VMSTATE_UINT32(pio_y, SGIMgrasState),
        VMSTATE_UINT32(cmd_token_class, SGIMgrasState),
        VMSTATE_UINT32(cmd_token_reg, SGIMgrasState),
        VMSTATE_UINT32(cmd_token_words_remaining, SGIMgrasState),
        VMSTATE_UINT32_SUB_ARRAY(cmd_token_payload, SGIMgrasState, 0, 8),
        VMSTATE_UINT32(cmd_token_payload_count, SGIMgrasState),
        VMSTATE_UINT16_ARRAY(vc3_reg, SGIMgrasState, 32),
        VMSTATE_UINT8(vc3_index, SGIMgrasState),
        VMSTATE_UINT16(vc3_ram_addr, SGIMgrasState),
        VMSTATE_UINT16_ARRAY(vc3_sram, SGIMgrasState,
                             SGI_MGRAS_VC3_SRAM_WORDS),
        VMSTATE_UINT8(xmap_index, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(xmap_mode, SGIMgrasState,
                             SGI_MGRAS_XMAP_ENTRIES),
        VMSTATE_UINT32_ARRAY_V(xmap_buf_select, SGIMgrasState,
                               SGI_MGRAS_XMAP_ENTRIES, 11),
        VMSTATE_TIMER_PTR_V(retrace_timer, SGIMgrasState, 2),
        VMSTATE_INT32_ARRAY_V(gl_context[0].viewport, SGIMgrasState, 4, 15),
        VMSTATE_UINT32_ARRAY_V(gl_context[0].clear_color, SGIMgrasState, 4,
                               14),
        VMSTATE_UINT32_ARRAY_V(gl_context[0].color, SGIMgrasState, 4, 15),
        VMSTATE_UINT32_V(gl_context[0].shade_model, SGIMgrasState, 15),
        VMSTATE_UINT32_V(gl_context[0].begin, SGIMgrasState, 15),
        VMSTATE_UINT32_V(gl_context[0].vertex_count, SGIMgrasState, 15),
        VMSTATE_UINT32_2DARRAY_V(gl_context[0].vertex, SGIMgrasState, 3, 4,
                                 15),
        VMSTATE_UINT32_2DARRAY_V(gl_context[0].vertex_color, SGIMgrasState,
                                 3, 4, 15),
        VMSTATE_UINT32_V(ge_report, SGIMgrasState, 16),
        /*
         * Version 17 grows the command payload to 16 words for
         * glLoadMatrixf; an older stream carries the first 8, and a packet
         * it left open had no more than that.
         */
        VMSTATE_SUB_ARRAY(cmd_token_payload, SGIMgrasState, 8, 8, 17,
                          vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32_V(gl_context[0].matrix_mode, SGIMgrasState, 17),
        VMSTATE_UINT32_ARRAY_V(gl_context[0].modelview, SGIMgrasState, 16,
                               17),
        VMSTATE_UINT32_ARRAY_V(gl_context[0].projection, SGIMgrasState, 16,
                               17),
        VMSTATE_UINT32_V(gl_context[0].depth_test, SGIMgrasState, 17),
        VMSTATE_UINT32_V(gl_context[0].depth_func, SGIMgrasState, 17),
        VMSTATE_UINT32_V(gl_context[0].depth_mask, SGIMgrasState, 17),
        VMSTATE_UINT32_V(gl_context[0].clear_depth, SGIMgrasState, 17),
        VMSTATE_UINT32_V(gl_context[0].draw_buffers, SGIMgrasState, 17),
        VMSTATE_UINT32_ARRAY_V(gl_banks, SGIMgrasState, 2, 17),
        VMSTATE_VARRAY_UINT32(depth, SGIMgrasState, depth_count, 17,
                              vmstate_info_uint32, uint32_t),
        /*
         * Versions 14 to 17 kept one GL state for the board, sent above as
         * entry 0, the state used with no GL context loaded. Version 18
         * adds the per-context entries.
         */
        VMSTATE_STRUCT_SUB_ARRAY(gl_context, SGIMgrasState, 1,
                                 SGI_MGRAS_GL_CONTEXTS - 1, 18,
                                 vmstate_sgi_mgras_gl_context,
                                 SGIMgrasGLContext),
        VMSTATE_UINT32_ARRAY_V(gl_slot, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS,
                               18),
        VMSTATE_UINT64_ARRAY_V(gl_selected, SGIMgrasState,
                               SGI_MGRAS_GL_CONTEXTS, 18),
        VMSTATE_UINT64_V(gl_selections, SGIMgrasState, 18),
        VMSTATE_UINT32_V(gl_current, SGIMgrasState, 18),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mgras_properties[] = {
    DEFINE_PROP_UINT32("revision", SGIMgrasState, revision, 0),
    DEFINE_PROP_UINT32("part", SGIMgrasState, part, 0xc003),
    DEFINE_PROP_UINT32("manufacturer", SGIMgrasState, manufacturer, 0x02aa),
    DEFINE_PROP_STRING("board", SGIMgrasState, board_name),
    /* Present a board the model cannot render; for identity experiments. */
    DEFINE_PROP_BOOL("x-unimplemented-board", SGIMgrasState,
                     unimplemented_board, false),
    DEFINE_PROP_LINK("fabric", SGIMgrasState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static bool mgras_test_get_first_ack(Object *obj, Error **errp)
{
    return SGI_MGRAS(obj)->first_ack;
}

static bool mgras_test_get_second_ack(Object *obj, Error **errp)
{
    return SGI_MGRAS(obj)->second_ack;
}

/* Host-only qtest access to native pixel storage. */
static void mgras_test_get_pixel(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized && s->test_pixel_x < s->width &&
        s->test_pixel_y < s->height) {
        value = s->front[s->test_pixel_y * s->width + s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

/* Update scanout before exposing a display pixel to qtests. */
static void mgras_test_get_display_pixel(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized) {
        qemu_console_hw_update(s->con);
    }
    if (DEVICE(obj)->realized && s->test_pixel_x < s->width &&
        s->test_pixel_y < s->height) {
        value = s->display_buffer[s->test_pixel_y * s->width +
                                  s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

/* Expose the main-plane window ID scanout uses, for replay diagnostics. */
static void mgras_test_get_wid(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized) {
        value = sgi_mgras_display_did(s, false, s->test_pixel_x,
                                      s->test_pixel_y);
    }
    visit_type_uint32(v, name, &value, errp);
}

/* Expose the current model per-pixel colour-format tag for replay diagnostics. */
static void mgras_test_get_format(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized && s->test_pixel_x < s->width &&
        s->test_pixel_y < s->height) {
        value = s->format[s->test_pixel_y * s->width + s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

/* Expose the overlay index for replay diagnostics. */
static void mgras_test_get_overlay(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized && s->test_pixel_x < s->width &&
        s->test_pixel_y < s->height) {
        value = s->overlay[s->test_pixel_y * s->width +
                           s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only qtest access to the DMA source-mapping slot x-test-dma-map-slot
 * currently selects.
 */
static void mgras_test_get_dma_map_base(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint64_t value = sgi_mgras_dma_map_slot_base(s, s->test_dma_map_slot);

    visit_type_uint64(v, name, &value, errp);
}

static void mgras_test_get_dma_map_count(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = sgi_mgras_dma_map_slot_count(s, s->test_dma_map_slot);

    visit_type_uint32(v, name, &value, errp);
}

static void mgras_test_get_dma_map_width(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = sgi_mgras_dma_map_slot_width(s, s->test_dma_map_slot);

    visit_type_uint32(v, name, &value, errp);
}

static void mgras_test_get_dma_map_control(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = sgi_mgras_dma_map_slot_control(s, s->test_dma_map_slot);

    visit_type_uint32(v, name, &value, errp);
}

static void mgras_instance_init(Object *obj)
{
    SGIMgrasState *s = SGI_MGRAS(obj);

    object_property_add_uint32_ptr(obj, "x-test-pixel-x", &s->test_pixel_x,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-pixel-y", &s->test_pixel_y,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-pixel", "uint32", mgras_test_get_pixel,
                        NULL, NULL, NULL);
    object_property_add(obj, "x-test-display-pixel", "uint32",
                        mgras_test_get_display_pixel, NULL, NULL, NULL);
    object_property_add(obj, "x-test-wid", "uint32", mgras_test_get_wid,
                        NULL, NULL, NULL);
    object_property_add(obj, "x-test-format", "uint32", mgras_test_get_format,
                        NULL, NULL, NULL);
    object_property_add(obj, "x-test-overlay", "uint32",
                        mgras_test_get_overlay, NULL, NULL, NULL);
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
                             mgras_test_get_first_ack, NULL);
    object_property_add_bool(obj, "x-test-second-ack",
                             mgras_test_get_second_ack, NULL);
    object_property_add_uint32_ptr(obj, "x-test-int-dest-upper",
                                   &s->int_dest_upper, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-int-dest-lower",
                                   &s->int_dest_lower, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-dma-map-slot",
                                   &s->test_dma_map_slot,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-dma-map-base", "uint64",
                        mgras_test_get_dma_map_base, NULL, NULL, NULL);
    object_property_add(obj, "x-test-dma-map-count", "uint32",
                        mgras_test_get_dma_map_count, NULL, NULL, NULL);
    object_property_add(obj, "x-test-dma-map-width", "uint32",
                        mgras_test_get_dma_map_width, NULL, NULL, NULL);
    object_property_add(obj, "x-test-dma-map-control", "uint32",
                        mgras_test_get_dma_map_control, NULL, NULL, NULL);
}

static void mgras_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SGIMgrasClass *mc = SGI_MGRAS_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    bc->build_widget = mgras_build_widget;
    bc->finish_realize = mgras_finish_realize;
    device_class_set_parent_unrealize(dc, mgras_unrealize,
                                      &mc->parent_unrealize);
    resettable_class_set_parent_phases(rc, mgras_reset_enter, mgras_reset_hold,
                                       mgras_reset_exit, &mc->parent_phases);
    dc->vmsd = &vmstate_sgi_mgras;
    dc->desc = "SGI MGRAS (SI/MXI) graphics XIO board";
    /*
     * Ordinarily fitted by the catalogue, and also -device-creatable at an
     * empty XIO slot it names (slot=io3), which hands it that slot's
     * fabric (xio-board.c).
     */
    dc->user_creatable = true;
    device_class_set_props(dc, mgras_properties);
}

static const TypeInfo mgras_types[] = {{
    .name = TYPE_SGI_MGRAS, .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIMgrasState), .class_size = sizeof(SGIMgrasClass),
    .instance_init = mgras_instance_init,
    .class_init = mgras_class_init }};
DEFINE_TYPES(mgras_types)
