/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI MediaIO RAD1 digital/analog/genlock audio ASIC.
 *
 * Register-file bring-up, the free-running UST counter, the status DMA, and
 * the analog output's streaming engine (channel type 1). IRIX's rad_dd.o
 * programs a documented set of BAR0 offsets during rad_start_subsys, gated
 * by the board mode it reads from the PCI revision byte (see the header),
 * so this model needs no mode-selection logic of its
 * own -- it accepts whatever the guest's mode-driven dispatch writes and
 * answers with the value last stored there. Modeled pieces of the audio data
 * path: the free-running UST counter at 0x30, whose advancing value clears
 * the driver's "RAD UST not incrementing" report; the status DMA, which
 * fills the driver's host-memory descriptor-ring slots with a register
 * mirror, each running channel's DMA pointer and a completion word; and the
 * analog output, which from its enable edge walks a guest ring buffer at
 * the sample rate, feeds a host audio backend and advances its position
 * registers. The analog input, the digital channels, genlock and the
 * interface-parameter registers are not modeled -- those registers accept
 * and shadow writes with no signal path. See
 * docs/datasheets/mediaio-rad1-audio.md.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/audio/sgi-rad1-audio.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "trace.h"

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

static void sgi_rad1_audio_out_sync(SGIRad1AudioState *s);

/* The analog output's current DMA pointer, low 32 bits of the PCI address. */
static uint32_t sgi_rad1_audio_out_pointer(SGIRad1AudioState *s)
{
    return (uint32_t)(s->out_base + s->out_offset);
}

/*
 * Fill one descriptor-ring slot, as a completion of the status DMA.
 *
 * - +0x44..+0x74: registers 0x04..0x34, the position bank, the 0x30 counter
 *   and the sync status, as they read now. The output engine is brought up
 *   to the present first, so the positions and pointer are those of this
 *   completion, not of the engine's last timer step.
 * - +0x38: the analog output's DMA pointer, once the channel has been
 *   started since reset. The other channels' pointer words and the odd
 *   words are left alone: no other channel runs, and the driver does not
 *   read the odd words.
 * - +0x78 and +0x7c: no ADAT, AES3 or video reference, and done. The done
 *   word goes last, so the driver never sees a done slot with a stale body.
 *
 * Every word goes through the device's own PCI DMA address space, written
 * little-endian. The RAD1 is a little-endian PCI device that is not
 * byte-lane self-ordered, so the Bridge byte-reverses its DMA into the
 * big-endian host memory the driver reads with plain loads; writing
 * big-endian here would put the done bit in the low byte.
 */
static bool sgi_rad1_audio_fill_slot(SGIRad1AudioState *s, uint64_t slot)
{
    PCIDevice *dev = PCI_DEVICE(s);
    uint8_t mirror[SGI_RAD1_AUDIO_SLOT_MIRROR_WORDS * 4];
    uint8_t word[4];
    unsigned int i;

    sgi_rad1_audio_out_sync(s);

    for (i = 0; i < SGI_RAD1_AUDIO_SLOT_MIRROR_WORDS; i++) {
        hwaddr reg = SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST + i * 4;
        uint32_t value;

        if (reg <= SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST) {
            value = s->sample_pos[SGI_RAD1_AUDIO_POS_IDX(reg)];
        } else if (reg == SGI_RAD1_AUDIO_REG_UST_COUNTER) {
            value = sgi_rad1_audio_ust_counter(s);
        } else {
            value = SGI_RAD1_AUDIO_SYNC_STATUS;
        }
        stl_le_p(mirror + i * 4, value);
    }
    if (pci_dma_write(dev, slot + SGI_RAD1_AUDIO_SLOT_MIRROR_OFFSET +
                      SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST,
                      mirror, sizeof(mirror)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: status-DMA slot write at 0x%" PRIx64
                      " failed; ring base unmapped?\n", slot);
        return false;
    }

    if (s->out_len) {
        stl_le_p(word, sgi_rad1_audio_out_pointer(s));
        pci_dma_write(dev, slot + SGI_RAD1_AUDIO_SLOT_OUT_PTR_OFFSET,
                      word, sizeof(word));
    }

    stl_le_p(word, SGI_RAD1_AUDIO_SLOT_FLAGS_WORD);
    pci_dma_write(dev, slot + SGI_RAD1_AUDIO_SLOT_FLAGS_OFFSET,
                  word, sizeof(word));
    stl_le_p(word, SGI_RAD1_AUDIO_SLOT_DONE_WORD);
    pci_dma_write(dev, slot + SGI_RAD1_AUDIO_SLOT_DONE_OFFSET,
                  word, sizeof(word));
    return true;
}

/* The descriptor ring's host-memory PCI base, from the status base regs. */
static uint64_t sgi_rad1_audio_status_ring_base(SGIRad1AudioState *s)
{
    uint32_t base_lo = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_STATUS_BASE_LO)];
    uint32_t base_hi = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_STATUS_BASE_HI)];

    return ((uint64_t)base_hi << 32) | base_lo;
}

/*
 * The ring's slot count, recovered from the limit register.
 * rad_start_status_dma encodes it as LIMIT = 0x78 - count * 0x80. A limit that
 * does not decode to a whole ring falls back to a single slot, so a caller that
 * programmed the ring differently than the driver still sees a completion.
 */
static unsigned int sgi_rad1_audio_status_ring_slots(SGIRad1AudioState *s)
{
    uint32_t limit = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_STATUS_LIMIT)];
    int64_t span = (int64_t)SGI_RAD1_AUDIO_LIMIT_BASE - (int32_t)limit;
    unsigned int slots;

    if (span >= SGI_RAD1_AUDIO_SLOT_STRIDE) {
        slots = span / SGI_RAD1_AUDIO_SLOT_STRIDE;
        if (slots > SGI_RAD1_AUDIO_SLOT_MAX) {
            slots = SGI_RAD1_AUDIO_SLOT_MAX;
        }
    } else {
        slots = 1;
    }
    return slots;
}

/*
 * One-shot status transfer. rad_one_shot_status programs the ring base and a
 * -4 limit, writes 0x40001 to 0x5c, and busy-polls the base slot's +0x7c bit
 * 31. The engine fills that one slot, exactly as a continuous completion
 * would.
 */
static void sgi_rad1_audio_one_shot_status(SGIRad1AudioState *s)
{
    sgi_rad1_audio_fill_slot(s, sgi_rad1_audio_status_ring_base(s));
}

/* When completion k of the current schedule is due. */
static int64_t sgi_rad1_audio_status_deadline(SGIRad1AudioState *s,
                                              uint64_t k)
{
    return s->status_start_ns +
           muldiv64(k * s->status_interval, NANOSECONDS_PER_SECOND,
                    SGI_RAD1_AUDIO_STATUS_CLOCK_HZ);
}

/*
 * Continuous-engine tick: fill the next slot and re-arm for the next
 * deadline on the absolute schedule. The cursor advances in ring order with
 * wrap: the driver's servicer consumes consecutive done slots from just
 * past its own consumer index and clears each done bit, so filling the
 * next slot each completion keeps it finding fresh ones.
 */
static void sgi_rad1_audio_status_tick(void *opaque)
{
    SGIRad1AudioState *s = opaque;
    uint64_t base = sgi_rad1_audio_status_ring_base(s);
    uint64_t slot = base + (uint64_t)s->status_ring_cursor *
                    SGI_RAD1_AUDIO_SLOT_STRIDE;
    int64_t now;

    if (!s->status_running) {
        return;
    }

    if (!sgi_rad1_audio_fill_slot(s, slot)) {
        /* Ring base unmapped: stop rather than spin on a failing DMA. */
        s->status_running = false;
        return;
    }

    if (s->status_ring_slots) {
        s->status_ring_cursor =
            (s->status_ring_cursor + 1) % s->status_ring_slots;
    }
    s->status_done++;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now > sgi_rad1_audio_status_deadline(s, s->status_done +
                                             s->status_ring_slots)) {
        s->status_start_ns = now;
        s->status_done = 0;
    }
    timer_mod(s->status_timer,
              sgi_rad1_audio_status_deadline(s, s->status_done + 1));
}

/*
 * Start the continuous engine. rad_start_status_dma resets the consumer index
 * to 0 and starts producing; the model starts its cursor one past that (the
 * first slot the servicer checks) and schedules the first completion one
 * interval out. A zero interval names no period, so it stops the engine
 * rather than completing slots as fast as the host can; an interval below
 * the floor is raised to it (see the header).
 */
static void sgi_rad1_audio_status_ring_start(SGIRad1AudioState *s,
                                             uint32_t ctrl)
{
    uint32_t interval = (ctrl >> SGI_RAD1_AUDIO_STATUS_INTERVAL_SHIFT) &
                        SGI_RAD1_AUDIO_STATUS_INTERVAL_MASK;

    if (!interval) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: continuous status DMA started with "
                      "a zero interval (control 0x%x); stopping\n", ctrl);
        s->status_running = false;
        timer_del(s->status_timer);
        return;
    }
    if (interval < SGI_RAD1_AUDIO_STATUS_INTERVAL_MIN) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: status-DMA interval %u is below the "
                      "modeled floor; using %u\n",
                      interval, SGI_RAD1_AUDIO_STATUS_INTERVAL_MIN);
        interval = SGI_RAD1_AUDIO_STATUS_INTERVAL_MIN;
    }
    s->status_ring_slots = sgi_rad1_audio_status_ring_slots(s);
    s->status_ring_cursor = s->status_ring_slots > 1 ? 1 : 0;
    s->status_interval = interval;
    s->status_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->status_done = 0;
    s->status_running = true;
    timer_mod(s->status_timer, sgi_rad1_audio_status_deadline(s, 1));
}

/* Stop the continuous engine: rad_stop_status_dma's 0x1fffc, or a reset. */
static void sgi_rad1_audio_status_ring_stop(SGIRad1AudioState *s)
{
    s->status_running = false;
    if (s->status_timer) {
        timer_del(s->status_timer);
    }
}

/*
 * The backend calls this when the output voice has room for more samples.
 * The engine pushes on its own timer instead, so nothing is written here.
 */
static void sgi_rad1_audio_out_callback(void *opaque, int avail)
{
}

/* Fixed output-voice format; see the header for why it is an approximation. */
static const struct audsettings sgi_rad1_audio_out_settings = {
    .freq = SGI_RAD1_AUDIO_OUT_FREQ,
    .nchannels = SGI_RAD1_AUDIO_OUT_CHANNELS,
    .fmt = AUDIO_FORMAT_S16,
    .big_endian = false,
};

static void sgi_rad1_audio_out_set_voice(SGIRad1AudioState *s, bool on)
{
    if (s->audio_be && s->dac_voice) {
        audio_be_set_active_out(s->audio_be, s->dac_voice, on);
    }
}

/*
 * Credit frames to type 1's position registers. Both count frames, two
 * ring words each: the datasheet's flagged default.
 *
 * A, 0x2c: the frame count in bits [31:18], with natural 32-bit wrap, and
 * the 0x30 counter mod 2^18 latched into bits [17:0] as the count moves.
 * rad_sync_device_ust splices those low bits into its UST.
 *
 * B, 0x18: type 1 reads bits [17:4], and rad_start_dma's post-enable poll
 * waits for them to change. The field counts the same frames, mod 2^14.
 * Type 0 owns bits [31:18] of the same register, so those and bits [3:0]
 * are left as they are.
 */
static void sgi_rad1_audio_out_advance_pos(SGIRad1AudioState *s,
                                           uint32_t frames)
{
    uint32_t *pos_a = &s->sample_pos[SGI_RAD1_AUDIO_POS_IDX(
        SGI_RAD1_AUDIO_REG_OUT_POS_A)];
    uint32_t *pos_b = &s->sample_pos[SGI_RAD1_AUDIO_POS_IDX(
        SGI_RAD1_AUDIO_REG_OUT_POS_B)];
    uint32_t field_mask = SGI_RAD1_AUDIO_POS_FIELD_MASK <<
                          SGI_RAD1_AUDIO_OUT_POS_B_SHIFT;
    uint32_t b_field;

    *pos_a = ((*pos_a & ~SGI_RAD1_AUDIO_UST_LATCH_MASK) +
              (frames << SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT)) |
             (sgi_rad1_audio_ust_counter(s) & SGI_RAD1_AUDIO_UST_LATCH_MASK);

    b_field = ((*pos_b >> SGI_RAD1_AUDIO_OUT_POS_B_SHIFT) + frames) &
              SGI_RAD1_AUDIO_POS_FIELD_MASK;
    *pos_b = (*pos_b & ~field_mask) |
             (b_field << SGI_RAD1_AUDIO_OUT_POS_B_SHIFT);

    trace_sgi_rad1_audio_out_position(frames, *pos_a, *pos_b);
}

static void sgi_rad1_audio_out_stop(SGIRad1AudioState *s)
{
    if (s->out_timer) {
        timer_del(s->out_timer);
    }
    if (!s->out_running) {
        return;
    }
    s->out_running = false;
    sgi_rad1_audio_out_set_voice(s, false);
    trace_sgi_rad1_audio_out_stop(sgi_rad1_audio_out_pointer(s),
                                  s->out_frames);
}

/*
 * Move frames from the ring to the voice, wrapping at the ring's end. The
 * words are read through the device's PCI DMA address space, so the
 * Bridge's translation and byte-lane reversal apply, and each 24-bit
 * channel sample becomes a 16-bit one from its bits [23:8]. The read
 * happens whatever the backend accepts, so the pointer and positions
 * advance the same with or without an audio device; a backend that takes
 * less drops the rest.
 */
static bool sgi_rad1_audio_out_move(SGIRad1AudioState *s, uint64_t frames)
{
    PCIDevice *dev = PCI_DEVICE(s);
    uint8_t buf[1024];
    uint8_t pcm[sizeof(buf) / 2];

    while (frames) {
        uint64_t bytes = MIN(frames * SGI_RAD1_AUDIO_OUT_FRAME_BYTES,
                             s->out_len - s->out_offset);
        size_t chunk = MIN(bytes, sizeof(buf));

        if (pci_dma_read(dev, s->out_base + s->out_offset, buf,
                         chunk) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-rad1-audio: output DMA read at 0x%" PRIx64
                          " failed; ring address unmapped?\n",
                          s->out_base + s->out_offset);
            return false;
        }
        if (s->audio_be && s->dac_voice) {
            size_t words = chunk / SGI_RAD1_AUDIO_OUT_WORD_BYTES;
            size_t i;

            for (i = 0; i < words; i++) {
                uint32_t word = ldl_le_p(buf + i * 4);

                stw_le_p(pcm + i * 2,
                         (uint16_t)(word >> SGI_RAD1_AUDIO_OUT_SAMPLE_SHIFT));
            }
            audio_be_write(s->audio_be, s->dac_voice, pcm, words * 2);
        }
        s->out_offset += chunk;
        frames -= chunk / SGI_RAD1_AUDIO_OUT_FRAME_BYTES;
        if (s->out_offset >= s->out_len) {
            s->out_offset = 0;
            trace_sgi_rad1_audio_out_wrap(s->out_base);
        }
    }
    return true;
}

/*
 * Bring the engine up to the present. The frames due are those the sample
 * clock has produced since the enable edge; computing them from elapsed
 * virtual time keeps the rate exact however late a timer fires. A status
 * completion calls this too, so its slot carries the pointer as of that
 * completion.
 */
static void sgi_rad1_audio_out_sync(SGIRad1AudioState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t due, frames;

    /* Elapsed time at or below zero would become a huge unsigned count. */
    if (!s->out_running || now <= s->out_start_ns) {
        return;
    }

    due = muldiv64(now - s->out_start_ns, SGI_RAD1_AUDIO_OUT_FREQ,
                   NANOSECONDS_PER_SECOND);
    if (due > s->out_frames) {
        frames = due - s->out_frames;
        if (!sgi_rad1_audio_out_move(s, frames)) {
            sgi_rad1_audio_out_stop(s);
            return;
        }
        s->out_frames = due;
        sgi_rad1_audio_out_advance_pos(s, (uint32_t)frames);
    }
}

/* Engine step: catch up, then re-arm OUT_TICK_FRAMES frames on. */
static void sgi_rad1_audio_out_tick(void *opaque)
{
    SGIRad1AudioState *s = opaque;

    sgi_rad1_audio_out_sync(s);
    if (!s->out_running) {
        return;
    }

    /* Round up, so the frames due when it fires are never one short. */
    timer_mod(s->out_timer, s->out_start_ns +
              muldiv64_round_up(s->out_frames +
                                SGI_RAD1_AUDIO_OUT_TICK_FRAMES,
                                NANOSECONDS_PER_SECOND,
                                SGI_RAD1_AUDIO_OUT_FREQ));
}

/*
 * The enable edge. rad_start_dma programs set A and set B with the same ring
 * and control word, then sets bit 0 of 0x58; that write alone starts the
 * channel. The engine latches set A. Set B is stored but not consumed
 * separately: every captured start wrote it identical to set A, and the
 * datasheet only infers that the hardware alternates between the two.
 */
static void sgi_rad1_audio_out_start(SGIRad1AudioState *s)
{
    uint32_t addr_lo = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_OUT_A_ADDR_LO)];
    uint32_t addr_hi = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_OUT_A_ADDR_HI)];
    uint32_t ctrl = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(
        SGI_RAD1_AUDIO_REG_OUT_A_CTRL)];
    uint64_t base = ((uint64_t)addr_hi << 32) | addr_lo;
    uint32_t neg = (ctrl >> SGI_RAD1_AUDIO_CTRL_LEN_SHIFT) &
                   SGI_RAD1_AUDIO_CTRL_LEN_MASK;
    uint32_t units = (0u - neg) & SGI_RAD1_AUDIO_CTRL_LEN_MASK;
    uint64_t len = (uint64_t)units * SGI_RAD1_AUDIO_DMA_UNIT_BYTES;
    uint32_t tag = (ctrl >> SGI_RAD1_AUDIO_CTRL_TAG_SHIFT) &
                   SGI_RAD1_AUDIO_CTRL_TAG_MASK;

    if (base == 0 || len == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: analog output enabled with no ring "
                      "programmed (address 0x%" PRIx64 ", control 0x%x)\n",
                      base, ctrl);
        return;
    }
    if (len > SGI_RAD1_AUDIO_DMA_MAX_BYTES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: output ring length 0x%" PRIx64
                      " exceeds the modeled cap; clamping\n", len);
        len = SGI_RAD1_AUDIO_DMA_MAX_BYTES;
    }
    if (tag != SGI_RAD1_AUDIO_OUT_TAG) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: analog output control word 0x%x "
                      "carries tag %u, not the channel type %u\n",
                      ctrl, tag, SGI_RAD1_AUDIO_OUT_TAG);
    }

    s->out_base = base;
    s->out_len = len;
    s->out_offset = 0;
    s->out_frames = 0;
    s->out_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->out_running = true;
    sgi_rad1_audio_out_set_voice(s, true);
    trace_sgi_rad1_audio_out_start(base, s->out_len, tag);
    timer_mod(s->out_timer, s->out_start_ns +
              muldiv64_round_up(SGI_RAD1_AUDIO_OUT_TICK_FRAMES,
                                NANOSECONDS_PER_SECOND,
                                SGI_RAD1_AUDIO_OUT_FREQ));
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
         * The per-channel sample-position bank. Each register reads zero
         * until a channel's engine advances it; only the analog output's pair
         * (0x2c, 0x18) moves. Read-only, so a guest write never changes it.
         * The output engine catches up first, so a read agrees with a status
         * slot filled at the same instant.
         */
        sgi_rad1_audio_out_sync(s);
        return s->sample_pos[SGI_RAD1_AUDIO_POS_IDX(addr)];
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
    case SGI_RAD1_AUDIO_REG_ANALOG_OUT_EN:
        return s->analog_out_en;
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
    case SGI_RAD1_AUDIO_REG_ANALOG_OUT_EN:
        /*
         * Bit 0 starts and stops the analog output. Bring-up writes
         * 0x20000000 with it clear; rad_start_dma sets it and rad_stop_dma
         * clears it, keeping bit 29 from the shadow. Rewriting a set bit
         * while the engine runs does not restart it.
         */
        s->analog_out_en = val;
        if (val & SGI_RAD1_AUDIO_OUT_ENABLE_BIT) {
            if (!s->out_running) {
                sgi_rad1_audio_out_start(s);
            }
        } else {
            /* Stop where the sample clock has got to, not the last step. */
            sgi_rad1_audio_out_sync(s);
            sgi_rad1_audio_out_stop(s);
        }
        break;
    case SGI_RAD1_AUDIO_REG_STATUS_DMA_CTRL:
        s->status_dma_ctrl = val;
        /*
         * The ring bit (0x2) tells the continuous ring engine (0x40003) from
         * the one-shot (0x40001); the interval field in bits [16:2] is masked
         * off first. The continuous start arms the periodic completion timer;
         * the one-shot marks a single slot now; the stop word (0x1fffc) halts
         * the timer; any other write only records the register.
         */
        switch (val & SGI_RAD1_AUDIO_STATUS_CTRL_MASK) {
        case SGI_RAD1_AUDIO_STATUS_START:      /* 0x40003: continuous */
            sgi_rad1_audio_status_ring_start(s, val);
            break;
        case SGI_RAD1_AUDIO_STATUS_ONESHOT_START: /* 0x40001: one-shot */
            sgi_rad1_audio_one_shot_status(s);
            break;
        default:
            if (val == SGI_RAD1_AUDIO_STATUS_STOP) {
                sgi_rad1_audio_status_ring_stop(s);
            }
            break;
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
        /*
         * Central control: ADAT-receiver and clock-generator bits. No
         * channel's transfer starts or waits on it.
         */
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
    s->analog_out_en = 0;
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

    /* Stop the output engine and forget its ring; the voice stays open. */
    sgi_rad1_audio_out_stop(s);
    s->out_base = 0;
    s->out_len = 0;
    s->out_offset = 0;
    s->out_start_ns = 0;
    s->out_frames = 0;

    /* The sample-position bank reads zero until an engine advances it. */
    memset(s->sample_pos, 0, sizeof(s->sample_pos));

    /* Halt the continuous status-DMA engine and clear its cursor. */
    sgi_rad1_audio_status_ring_stop(s);
    s->status_ring_cursor = 0;
    s->status_ring_slots = 0;
    s->status_interval = 0;
    s->status_start_ns = 0;
    s->status_done = 0;

    /* The UST counter reads zero at reset and advances with the clock. */
    s->ust_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void sgi_rad1_audio_realize(PCIDevice *dev, Error **errp)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(dev);
    Error *audio_err = NULL;

    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);

    /*
     * The MediaIO board always instantiates this device, so a missing audio
     * backend must not abort machine creation. Resolve the audiodev property
     * (or the default); if none is available, warn and run without a voice --
     * the register file and the DMA position advance still work, only the
     * sample push to a host sink is skipped.
     */
    if (!audio_be_check(&s->audio_be, &audio_err)) {
        warn_report_err(audio_err);
        s->audio_be = NULL;
    }
    if (s->audio_be) {
        s->dac_voice = audio_be_open_out(s->audio_be, NULL,
                                         "sgi-rad1-audio.out", s,
                                         sgi_rad1_audio_out_callback,
                                         &sgi_rad1_audio_out_settings);
    }

    memory_region_init_io(&s->bar0, OBJECT(dev), &sgi_rad1_audio_bar0_ops, s,
                          "sgi-rad1-audio.bar0", SGI_RAD1_AUDIO_BAR0_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /* The continuous status-DMA completion timer; armed on a 0x40003 start. */
    s->status_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   sgi_rad1_audio_status_tick, s);
    /* The analog output's pacing timer; armed on the 0x58 enable edge. */
    s->out_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                sgi_rad1_audio_out_tick, s);
}

static void sgi_rad1_audio_exit(PCIDevice *dev)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(dev);

    if (s->status_timer) {
        timer_free(s->status_timer);
        s->status_timer = NULL;
    }
    if (s->out_timer) {
        timer_free(s->out_timer);
        s->out_timer = NULL;
    }
    if (s->audio_be && s->dac_voice) {
        audio_be_close_out(s->audio_be, s->dac_voice);
        s->dac_voice = NULL;
    }
}

/* The voice is not migrated; match its activity to the engine. */
static int sgi_rad1_audio_post_load(void *opaque, int version_id)
{
    SGIRad1AudioState *s = opaque;

    /* Refuse a stream whose ring geometry would misdirect DMA. */
    if (s->out_len > SGI_RAD1_AUDIO_DMA_MAX_BYTES ||
        s->out_len % SGI_RAD1_AUDIO_OUT_FRAME_BYTES ||
        s->out_offset % SGI_RAD1_AUDIO_OUT_FRAME_BYTES ||
        (s->out_len && s->out_offset >= s->out_len) ||
        (s->out_running && !s->out_len) ||
        s->status_ring_slots > SGI_RAD1_AUDIO_SLOT_MAX ||
        (s->status_ring_slots &&
         s->status_ring_cursor >= s->status_ring_slots) ||
        (s->status_running &&
         (!s->status_interval || !s->status_ring_slots))) {
        return -EINVAL;
    }
    sgi_rad1_audio_out_set_voice(s, s->out_running);
    return 0;
}

static const VMStateDescription vmstate_sgi_rad1_audio = {
    .name = "sgi-rad1-audio",
    .version_id = 6,
    .minimum_version_id = 6,
    .post_load = sgi_rad1_audio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_a, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_b, SGIRad1AudioState),
        VMSTATE_UINT32(shift_load_clock, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_c, SGIRad1AudioState),
        VMSTATE_UINT32(predispatch_d, SGIRad1AudioState),
        VMSTATE_UINT32(analog_out_en, SGIRad1AudioState),
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
        VMSTATE_UINT32_ARRAY(sample_pos, SGIRad1AudioState,
                             SGI_RAD1_AUDIO_SAMPLE_POS_WORDS),
        VMSTATE_TIMER_PTR(out_timer, SGIRad1AudioState),
        VMSTATE_BOOL(out_running, SGIRad1AudioState),
        VMSTATE_UINT64(out_base, SGIRad1AudioState),
        VMSTATE_UINT32(out_len, SGIRad1AudioState),
        VMSTATE_UINT32(out_offset, SGIRad1AudioState),
        VMSTATE_INT64(out_start_ns, SGIRad1AudioState),
        VMSTATE_UINT64(out_frames, SGIRad1AudioState),
        VMSTATE_TIMER_PTR(status_timer, SGIRad1AudioState),
        VMSTATE_UINT32(status_ring_cursor, SGIRad1AudioState),
        VMSTATE_UINT32(status_ring_slots, SGIRad1AudioState),
        VMSTATE_BOOL(status_running, SGIRad1AudioState),
        VMSTATE_UINT32(status_interval, SGIRad1AudioState),
        VMSTATE_INT64(status_start_ns, SGIRad1AudioState),
        VMSTATE_UINT64(status_done, SGIRad1AudioState),
        VMSTATE_INT64(ust_base_ns, SGIRad1AudioState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_rad1_audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SGIRad1AudioState, audio_be),
};

static void sgi_rad1_audio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI MediaIO RAD1 audio";
    pc->realize = sgi_rad1_audio_realize;
    pc->exit = sgi_rad1_audio_exit;
    pc->vendor_id = SGI_RAD1_AUDIO_VENDOR_ID;
    pc->device_id = SGI_RAD1_AUDIO_DEVICE_ID;
    pc->revision = SGI_RAD1_AUDIO_REVISION;
    pc->class_id = SGI_RAD1_AUDIO_CLASS_ID;
    rc->phases.hold = sgi_rad1_audio_reset_hold;
    dc->vmsd = &vmstate_sgi_rad1_audio;
    device_class_set_props(dc, sgi_rad1_audio_properties);
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
