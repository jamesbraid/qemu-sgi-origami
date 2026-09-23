/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI MediaIO RAD1 digital/analog/genlock audio ASIC.
 *
 * Register-file bring-up, the free-running UST counter, and the status-DMA
 * completion handshake. IRIX's rad_dd.o programs a documented set of BAR0
 * offsets during rad_start_subsys, gated by the board mode it reads from the
 * PCI revision byte (see the header), so this model needs no mode-selection
 * logic of its own -- it accepts whatever the guest's mode-driven dispatch
 * writes and answers with the value last stored there. Two pieces of the
 * audio data path are modeled: the free-running UST counter at 0x30, whose
 * advancing value clears the driver's "RAD UST not incrementing" report, and
 * the status-DMA engine's completion handshake, which marks the driver's
 * host-memory descriptor-ring slots done so its frame counters advance.
 * Actual audio sample streaming (a host audio backend, the main per-channel
 * DMA moving data, an advancing sample-position bank) is not modeled. See
 * docs/datasheets/mediaio-rad1-audio.md.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/audio/sgi-rad1-audio.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/*
 * The free-running UST counter at BAR0 0x30, derived from the virtual clock
 * rather than stored: the value is the virtual time elapsed since reset in
 * UST_NS_PER_TICK-nanosecond ticks, truncated to 32 bits like the hardware
 * register. rad_set_ust_offset samples it twice a fixed delay apart and warns
 * "RAD UST not incrementing" if it moved by fewer than three ticks; tying it
 * to the clock rather than returning a fixed or just-past-threshold value
 * keeps it a real counter, not a guest-specific reply.
 */
static uint32_t sgi_rad1_audio_ust_counter(SGIRad1AudioState *s)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->ust_base_ns;

    if (elapsed < 0) {
        elapsed = 0;
    }
    return (uint32_t)(elapsed / SGI_RAD1_AUDIO_UST_NS_PER_TICK);
}

/* Word index into the 0x400..0x528 DMA bank for a BAR0 offset. */
#define SGI_RAD1_AUDIO_DMA_IDX(off) \
    (((off) - SGI_RAD1_AUDIO_REG_DMA_BANK_FIRST) / 4)

/*
 * Status-DMA completion handshake. rad_start_status_dma (ring engine) and
 * rad_one_shot_status (single transfer) each program a host-memory
 * descriptor ring's PCI base into the status base registers and then write
 * a start value to 0x5c; the hardware then transfers status into the ring
 * and marks every slot it processes done -- bit 31 of the slot's +0x7c word
 * and the frame-completion bits in its +0x78 word. rad_one_shot_status
 * busy-polls the first slot's +0x7c bit 31, and rad_update_subsys walks the
 * ring counting slots whose +0x7c bit 31 is set, incrementing a frame
 * counter for each +0x78 completion bit; with no completed slots the driver
 * computes a zero rate and reports "RAD UST not incrementing". This routine
 * performs that marking so those counters advance.
 *
 * The done bit is the observable hardware contract; it is set because that
 * is what the engine does on a start, not as a guest-specific reply. The
 * exact meaning of the +0x78 bits (0x80 and 0x100) beyond "they bump the
 * driver's frame counters", and of the individual 0x40003/0x40001 control
 * bits, is not decoded (see docs/datasheets/mediaio-rad1-audio.md); the
 * known behavior -- mark the processed slots done -- is implemented and the
 * unknowns left as-is.
 */
static void sgi_rad1_audio_status_dma_complete(SGIRad1AudioState *s,
                                               uint32_t ctrl)
{
    PCIDevice *dev = PCI_DEVICE(s);
    uint32_t base_lo = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_STATUS_BASE_LO)];
    uint32_t base_hi = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_STATUS_BASE_HI)];
    uint32_t limit = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_STATUS_LIMIT)];
    uint64_t base = ((uint64_t)base_hi << 32) | base_lo;
    unsigned int slots, i;

    if (ctrl & SGI_RAD1_AUDIO_STATUS_RING) {
        /*
         * Ring engine: rad_start_status_dma encoded the ring's slot count in
         * the limit as LIMIT = 0x78 - count * 0x80. Recover it and clamp to
         * the ring's documented maximum. A limit that does not decode to a
         * whole slot still marks one slot, so a caller that programmed the
         * ring differently than the driver does still sees a completion.
         */
        int64_t span = (int64_t)SGI_RAD1_AUDIO_LIMIT_BASE - (int32_t)limit;

        if (span >= SGI_RAD1_AUDIO_SLOT_STRIDE) {
            slots = span / SGI_RAD1_AUDIO_SLOT_STRIDE;
            if (slots > SGI_RAD1_AUDIO_SLOT_MAX) {
                slots = SGI_RAD1_AUDIO_SLOT_MAX;
            }
        } else {
            slots = 1;
        }
    } else {
        /* One-shot: a single transfer into the slot at the ring base. */
        slots = 1;
    }

    for (i = 0; i < slots; i++) {
        uint64_t slot = base + (uint64_t)i * SGI_RAD1_AUDIO_SLOT_STRIDE;
        uint8_t word[4];
        uint32_t value;

        /*
         * The guest reads the descriptor words with big-endian loads, so set
         * the bits in the guest's byte order. Read-modify-write goes through
         * the device's own PCI DMA address space (pci_dma_*), honoring the
         * Bridge's translation, rather than any core address space.
         */
        if (pci_dma_read(dev, slot + SGI_RAD1_AUDIO_SLOT_DONE_OFFSET,
                         word, sizeof(word)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-rad1-audio: status-DMA descriptor read at 0x%"
                          PRIx64 " failed; ring base unmapped?\n",
                          slot + SGI_RAD1_AUDIO_SLOT_DONE_OFFSET);
            return;
        }
        value = ldl_be_p(word) | SGI_RAD1_AUDIO_SLOT_DONE_BIT;
        stl_be_p(word, value);
        pci_dma_write(dev, slot + SGI_RAD1_AUDIO_SLOT_DONE_OFFSET,
                      word, sizeof(word));

        if (pci_dma_read(dev, slot + SGI_RAD1_AUDIO_SLOT_FLAGS_OFFSET,
                         word, sizeof(word)) != MEMTX_OK) {
            return;
        }
        value = ldl_be_p(word) | SGI_RAD1_AUDIO_SLOT_FRAME_BITS;
        stl_be_p(word, value);
        pci_dma_write(dev, slot + SGI_RAD1_AUDIO_SLOT_FLAGS_OFFSET,
                      word, sizeof(word));
    }
}

/*
 * BAR0: the documented register file. None of these offsets has an
 * independently confirmed reset value or read behavior, so reads answer
 * with whatever the guest last wrote -- a generic register-store
 * approximation, not a captured hardware contract. An offset this model
 * does not recognize logs as unimplemented rather than accepting a
 * silent store, so an undocumented access stays observable.
 */
static uint64_t sgi_rad1_audio_bar0_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    SGIRad1AudioState *s = opaque;

    if (addr >= SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST &&
        addr <= SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST) {
        /*
         * The per-channel sample-position bank. rad_start_dma reads it as the
         * MSC counter's hardware term. With no audio data path yet it reads a
         * fixed modeled value -- a stated approximation, not a captured
         * contract: the position advances only while audio DMA runs, which
         * this increment does not model, and it does not drive the "UST not
         * incrementing" report (the status-DMA handshake does). Read-only, so
         * the value is fixed rather than stored.
         */
        return SGI_RAD1_AUDIO_SAMPLE_POS_VALUE;
    }

    if (addr >= SGI_RAD1_AUDIO_REG_DMA_BANK_FIRST &&
        addr <= SGI_RAD1_AUDIO_REG_DMA_BANK_LAST) {
        return s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(addr)];
    }

    switch (addr) {
    case SGI_RAD1_AUDIO_REG_UST_COUNTER:
        return sgi_rad1_audio_ust_counter(s);
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_A:
        return s->predispatch_a;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_B:
        return s->predispatch_b;
    case SGI_RAD1_AUDIO_REG_SHIFT_LOAD_CLOCK:
        return s->shift_load_clock;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_C:
        return s->predispatch_c;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_D:
        return s->predispatch_d;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_E:
        return s->predispatch_e;
    case SGI_RAD1_AUDIO_REG_STATUS_DMA_CTRL:
        return s->status_dma_ctrl;
    case SGI_RAD1_AUDIO_REG_HALT_TEARDOWN:
        return s->halt_teardown;
    case SGI_RAD1_AUDIO_REG_BRINGUP_FIXED_A:
        return s->bringup_fixed_a;
    case SGI_RAD1_AUDIO_REG_BRINGUP_FIXED_B:
        return s->bringup_fixed_b;
    case SGI_RAD1_AUDIO_REG_MUX_CLOCK_SELECT:
        return s->mux_clock_select;
    case SGI_RAD1_AUDIO_REG_SHADOW_MASKED:
        return s->shadow_masked;
    case SGI_RAD1_AUDIO_REG_MODE_GATED_A:
        return s->mode_gated_a;
    case SGI_RAD1_AUDIO_REG_LATCH_PULSE:
        return s->latch_pulse;
    case SGI_RAD1_AUDIO_REG_SHIFT_LOAD_DATA:
        return s->shift_load_data;
    case SGI_RAD1_AUDIO_REG_MODE_GATED_B:
        return s->mode_gated_b;
    case SGI_RAD1_AUDIO_REG_BRINGUP_FINAL:
        return s->bringup_final;
    case SGI_RAD1_AUDIO_REG_CONVERGENCE_A:
        return s->convergence_a;
    case SGI_RAD1_AUDIO_REG_CONVERGENCE_B:
        return s->convergence_b;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-rad1-audio: read of BAR0 offset 0x%" HWADDR_PRIx
                      " which this model does not implement\n", addr);
        return 0;
    }
}

static void sgi_rad1_audio_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    SGIRad1AudioState *s = opaque;

    if (addr >= SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST &&
        addr <= SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST) {
        /* The sample-position bank is read-only; a write is a guest error. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: write of 0x%" PRIx64 " to read-only "
                      "sample-position register 0x%" HWADDR_PRIx "\n",
                      val, addr);
        return;
    }

    if (addr >= SGI_RAD1_AUDIO_REG_DMA_BANK_FIRST &&
        addr <= SGI_RAD1_AUDIO_REG_DMA_BANK_LAST) {
        s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(addr)] = val;
        return;
    }

    switch (addr) {
    case SGI_RAD1_AUDIO_REG_UST_COUNTER:
        /* The UST counter is read-only; a write is a guest error. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: write of 0x%" PRIx64 " to read-only "
                      "UST counter 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_A:
        s->predispatch_a = val;
        break;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_B:
        s->predispatch_b = val;
        break;
    case SGI_RAD1_AUDIO_REG_SHIFT_LOAD_CLOCK:
        s->shift_load_clock = val;
        break;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_C:
        s->predispatch_c = val;
        break;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_D:
        s->predispatch_d = val;
        break;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_E:
        s->predispatch_e = val;
        break;
    case SGI_RAD1_AUDIO_REG_STATUS_DMA_CTRL:
        s->status_dma_ctrl = val;
        /*
         * Start bits set (0x40003 ring engine or 0x40001 one-shot) kicks the
         * completion handshake; the stop value (0x1fffc) and any other write
         * only records the register.
         */
        if ((val & SGI_RAD1_AUDIO_STATUS_START_BITS) ==
            SGI_RAD1_AUDIO_STATUS_START_BITS) {
            sgi_rad1_audio_status_dma_complete(s, val);
        }
        break;
    case SGI_RAD1_AUDIO_REG_HALT_TEARDOWN:
        s->halt_teardown = val;
        break;
    case SGI_RAD1_AUDIO_REG_BRINGUP_FIXED_A:
        s->bringup_fixed_a = val;
        break;
    case SGI_RAD1_AUDIO_REG_BRINGUP_FIXED_B:
        s->bringup_fixed_b = val;
        break;
    case SGI_RAD1_AUDIO_REG_MUX_CLOCK_SELECT:
        s->mux_clock_select = val;
        break;
    case SGI_RAD1_AUDIO_REG_SHADOW_MASKED:
        s->shadow_masked = val;
        break;
    case SGI_RAD1_AUDIO_REG_MODE_GATED_A:
        s->mode_gated_a = val;
        break;
    case SGI_RAD1_AUDIO_REG_LATCH_PULSE:
        s->latch_pulse = val;
        break;
    case SGI_RAD1_AUDIO_REG_SHIFT_LOAD_DATA:
        s->shift_load_data = val;
        break;
    case SGI_RAD1_AUDIO_REG_MODE_GATED_B:
        s->mode_gated_b = val;
        break;
    case SGI_RAD1_AUDIO_REG_BRINGUP_FINAL:
        s->bringup_final = val;
        break;
    case SGI_RAD1_AUDIO_REG_CONVERGENCE_A:
        s->convergence_a = val;
        break;
    case SGI_RAD1_AUDIO_REG_CONVERGENCE_B:
        s->convergence_b = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-rad1-audio: write of 0x%" PRIx64
                      " to BAR0 offset 0x%" HWADDR_PRIx
                      " which this model does not implement\n", val, addr);
        break;
    }
}

static const MemoryRegionOps sgi_rad1_audio_bar0_ops = {
    .read = sgi_rad1_audio_bar0_read,
    .write = sgi_rad1_audio_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sgi_rad1_audio_reset_hold(Object *obj, ResetType type)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(obj);

    s->predispatch_a = 0;
    s->predispatch_b = 0;
    s->shift_load_clock = 0;
    s->predispatch_c = 0;
    s->predispatch_d = 0;
    s->predispatch_e = 0;
    s->status_dma_ctrl = 0;
    s->halt_teardown = 0;
    s->bringup_fixed_a = 0;
    s->bringup_fixed_b = 0;
    s->mux_clock_select = 0;
    s->shadow_masked = 0;
    s->mode_gated_a = 0;
    s->latch_pulse = 0;
    s->shift_load_data = 0;
    s->mode_gated_b = 0;
    s->bringup_final = 0;
    s->convergence_a = 0;
    s->convergence_b = 0;
    memset(s->dma_bank, 0, sizeof(s->dma_bank));

    /* The UST counter reads zero at reset and advances with the clock. */
    s->ust_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void sgi_rad1_audio_realize(PCIDevice *dev, Error **errp)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(dev);

    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);

    memory_region_init_io(&s->bar0, OBJECT(dev), &sgi_rad1_audio_bar0_ops, s,
                          "sgi-rad1-audio.bar0", SGI_RAD1_AUDIO_BAR0_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
}

static const VMStateDescription vmstate_sgi_rad1_audio = {
    .name = "sgi-rad1-audio",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_a, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_b, SGIRad1AudioState),
        VMSTATE_UINT32(shift_load_clock, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_c, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_d, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_e, SGIRad1AudioState),
        VMSTATE_UINT32(status_dma_ctrl, SGIRad1AudioState),
        VMSTATE_UINT32(halt_teardown, SGIRad1AudioState),
        VMSTATE_UINT32(bringup_fixed_a, SGIRad1AudioState),
        VMSTATE_UINT32(bringup_fixed_b, SGIRad1AudioState),
        VMSTATE_UINT32(mux_clock_select, SGIRad1AudioState),
        VMSTATE_UINT32(shadow_masked, SGIRad1AudioState),
        VMSTATE_UINT32(mode_gated_a, SGIRad1AudioState),
        VMSTATE_UINT32(latch_pulse, SGIRad1AudioState),
        VMSTATE_UINT32(shift_load_data, SGIRad1AudioState),
        VMSTATE_UINT32(mode_gated_b, SGIRad1AudioState),
        VMSTATE_UINT32(bringup_final, SGIRad1AudioState),
        VMSTATE_UINT32(convergence_a, SGIRad1AudioState),
        VMSTATE_UINT32(convergence_b, SGIRad1AudioState),
        VMSTATE_UINT32_ARRAY(dma_bank, SGIRad1AudioState,
                             SGI_RAD1_AUDIO_DMA_BANK_WORDS),
        VMSTATE_INT64(ust_base_ns, SGIRad1AudioState),
        VMSTATE_END_OF_LIST()
    }
};

static void sgi_rad1_audio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI MediaIO RAD1 audio";
    pc->realize = sgi_rad1_audio_realize;
    pc->vendor_id = SGI_RAD1_AUDIO_VENDOR_ID;
    pc->device_id = SGI_RAD1_AUDIO_DEVICE_ID;
    pc->revision = SGI_RAD1_AUDIO_REVISION;
    pc->class_id = SGI_RAD1_AUDIO_CLASS_ID;
    rc->phases.hold = sgi_rad1_audio_reset_hold;
    dc->vmsd = &vmstate_sgi_rad1_audio;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo sgi_rad1_audio_types[] = {
    {
        .name = TYPE_SGI_RAD1_AUDIO,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SGIRad1AudioState),
        .class_init = sgi_rad1_audio_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(sgi_rad1_audio_types)
