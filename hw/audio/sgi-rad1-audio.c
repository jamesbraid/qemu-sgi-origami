/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI MediaIO RAD1 audio registers, DMA, and host audio streams. */
/*
 * Register-file bring-up, the free-running UST counter, the status DMA, and
 * the audio channels' streaming engine:
 * analog input and output (channel types 0 and 1), AES3 input and output (2
 * and 3) and ADAT input and output (4 and
 * 5). IRIX's rad_dd.o programs a documented set of BAR0 offsets during
 * rad_start_subsys, gated by the board mode it
 * reads from the PCI revision byte (see the header), so this model needs no
 * mode-selection logic of its own. It accepts whatever the guest's
 * mode-driven dispatch writes and answers with
 * the value last stored there. Modeled
 * pieces of the audio data path: the free-running UST counter at 0x30, whose
 * advancing value clears the driver's
 * "RAD UST not incrementing" report; the status DMA, which fills the driver's
 * host-memory descriptor-ring slots
 * with a register mirror, each running channel's DMA pointer, and receiver
 * words that present the digital inputs
 * the host says are connected; and the six data channels, each of which from
 * its enable edge walks a guest ring
 * buffer at its sample rate, moving frames to or from a host audio backend,
 * and advances its position registers.
 * The subcode channels, genlock and the interface-parameter registers are not
 * modeled. Those registers and the output subcode windows accept and shadow
 * writes with no signal path.
 * The Octane variant supports its UST counter, bounded status DMA and
 * internal-clock analog output. Other channels and general board controls
 * remain unsupported.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/audio/sgi-rad1-audio.h"
#include "hw/audio/sgi-rad1-audio-pcm.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "trace.h"

/*
 * The free-running UST counter at BAR0 0x30, derived from the virtual clock
 * rather than stored: the value is the
 * virtual time elapsed since reset in UST_NS_PER_TICK-nanosecond ticks,
 * truncated to 32 bits like the hardware
 * register. rad_set_ust_offset samples it twice a fixed delay apart and warns
 * "RAD UST not incrementing" if it
 * moved by fewer than three ticks; tying it to the clock rather than
 * returning a fixed or just-past-threshold value
 * keeps it a real counter, not a guest-specific reply.
 */
static void sgi_rad1_octane_ust_update(SGIRad1AudioState *s);
static uint32_t sgi_rad1_octane_ust_at(SGIRad1AudioState *s, int64_t ns);

static uint32_t sgi_rad1_audio_ust_counter(SGIRad1AudioState *s)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->ust_base_ns;

    if (s->octane) {
        sgi_rad1_octane_ust_update(s);
        return s->octane_ust_count;
    }
    if (elapsed < 0) {
        elapsed = 0;
    }
    return (uint32_t)(elapsed / SGI_RAD1_AUDIO_UST_NS_PER_TICK);
}

/* Word index into the 0x400..0x528 DMA bank for a BAR0 offset. */
#define SGI_RAD1_AUDIO_DMA_IDX(off) \
    (((off) - SGI_RAD1_AUDIO_REG_DMA_BANK_FIRST) / 4)

/* The host interface a channel's voice belongs to, and so its audiodev. */
typedef enum SGIRad1AudioIface {
    SGI_RAD1_AUDIO_IFACE_ANALOG,
    SGI_RAD1_AUDIO_IFACE_AES,
    SGI_RAD1_AUDIO_IFACE_ADAT,
    SGI_RAD1_AUDIO_IFACE_NONE,   /* no host voice: subcode */
} SGIRad1AudioIface;

/*
 * What distinguishes one channel from another: its set-A registers, tag,
 * direction, position registers, status-slot
 * pointer word and frame width, all from the per-type table and the flagged
 * frame defaults, and its rate and host
 * interface (see the header). Set B is stored in the DMA bank but not
 * consumed: it matches set A on every known
 * start, and whether hardware alternates between the sets is unverified. A
 * rate of 0 means the AES3 input's, which
 * follows the connected source.
 */
typedef struct SGIRad1AudioChannelInfo {
    const char *name;
    const char *voice;
    bool output;
    uint32_t tag;
    hwaddr addr_lo;
    hwaddr addr_hi;
    hwaddr ctrl;
    uint32_t enable_bit;
    hwaddr pos_a;
    unsigned int pos_a_shift;
    hwaddr pos_b;
    unsigned int pos_b_shift;
    bool pos_b_parent;
    hwaddr slot_ptr;
    unsigned int words_per_frame;
    uint32_t freq;
    SGIRad1AudioIface iface;
} SGIRad1AudioChannelInfo;

static const SGIRad1AudioChannelInfo sgi_rad1_audio_channel_info[] = {
    [SGI_RAD1_AUDIO_CH_IN] = {
        .name = "analog input",
        .voice = "sgi-rad1-audio.analog-in",
        .output = false,
        .tag = SGI_RAD1_AUDIO_CH_IN,
        .addr_lo = SGI_RAD1_AUDIO_REG_IN_A_ADDR_LO,
        .addr_hi = SGI_RAD1_AUDIO_REG_IN_A_ADDR_HI,
        .ctrl = SGI_RAD1_AUDIO_REG_IN_A_CTRL,
        .enable_bit = SGI_RAD1_AUDIO_ENABLE_BIT,
        .pos_a = SGI_RAD1_AUDIO_REG_IN_POS_A,
        .pos_a_shift = SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT,
        .pos_b = SGI_RAD1_AUDIO_REG_ANALOG_POS_B,
        .pos_b_shift = SGI_RAD1_AUDIO_IN_POS_B_SHIFT,
        .slot_ptr = SGI_RAD1_AUDIO_SLOT_IN_PTR_OFFSET,
        .words_per_frame = SGI_RAD1_AUDIO_ANALOG_WORDS,
        .freq = SGI_RAD1_AUDIO_FREQ,
        .iface = SGI_RAD1_AUDIO_IFACE_ANALOG,
    },
    [SGI_RAD1_AUDIO_CH_OUT] = {
        .name = "analog output",
        .voice = "sgi-rad1-audio.analog-out",
        .output = true,
        .tag = SGI_RAD1_AUDIO_CH_OUT,
        .addr_lo = SGI_RAD1_AUDIO_REG_OUT_A_ADDR_LO,
        .addr_hi = SGI_RAD1_AUDIO_REG_OUT_A_ADDR_HI,
        .ctrl = SGI_RAD1_AUDIO_REG_OUT_A_CTRL,
        .enable_bit = SGI_RAD1_AUDIO_ENABLE_BIT,
        .pos_a = SGI_RAD1_AUDIO_REG_OUT_POS_A,
        .pos_a_shift = SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT,
        .pos_b = SGI_RAD1_AUDIO_REG_ANALOG_POS_B,
        .pos_b_shift = SGI_RAD1_AUDIO_OUT_POS_B_SHIFT,
        .slot_ptr = SGI_RAD1_AUDIO_SLOT_OUT_PTR_OFFSET,
        .words_per_frame = SGI_RAD1_AUDIO_ANALOG_WORDS,
        .freq = SGI_RAD1_AUDIO_FREQ,
        .iface = SGI_RAD1_AUDIO_IFACE_ANALOG,
    },
    [SGI_RAD1_AUDIO_CH_AES_IN] = {
        .name = "AES3 input",
        .voice = "sgi-rad1-audio.aes-in",
        .output = false,
        .tag = SGI_RAD1_AUDIO_CH_AES_IN,
        .addr_lo = SGI_RAD1_AUDIO_REG_AES_IN_A_ADDR_LO,
        .addr_hi = SGI_RAD1_AUDIO_REG_AES_IN_A_ADDR_HI,
        .ctrl = SGI_RAD1_AUDIO_REG_AES_IN_A_CTRL,
        .enable_bit = SGI_RAD1_AUDIO_ENABLE_BIT,
        .pos_a = SGI_RAD1_AUDIO_REG_AES_IN_POS_A,
        .pos_a_shift = SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT,
        .pos_b = SGI_RAD1_AUDIO_REG_AES_IN_POS_B,
        .pos_b_shift = SGI_RAD1_AUDIO_DIGITAL_POS_B_SHIFT,
        .slot_ptr = SGI_RAD1_AUDIO_SLOT_AES_IN_PTR_OFFSET,
        .words_per_frame = SGI_RAD1_AUDIO_AES_WORDS,
        .freq = 0,
        .iface = SGI_RAD1_AUDIO_IFACE_AES,
    },
    [SGI_RAD1_AUDIO_CH_AES_OUT] = {
        .name = "AES3 output",
        .voice = "sgi-rad1-audio.aes-out",
        .output = true,
        .tag = SGI_RAD1_AUDIO_CH_AES_OUT,
        .addr_lo = SGI_RAD1_AUDIO_REG_AES_OUT_A_ADDR_LO,
        .addr_hi = SGI_RAD1_AUDIO_REG_AES_OUT_A_ADDR_HI,
        .ctrl = SGI_RAD1_AUDIO_REG_AES_OUT_A_CTRL,
        .enable_bit = SGI_RAD1_AUDIO_ENABLE_BIT,
        .pos_a = SGI_RAD1_AUDIO_REG_AES_OUT_POS_A,
        .pos_a_shift = SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT,
        .pos_b = SGI_RAD1_AUDIO_REG_AES_OUT_POS_B,
        .pos_b_shift = SGI_RAD1_AUDIO_DIGITAL_POS_B_SHIFT,
        .slot_ptr = SGI_RAD1_AUDIO_SLOT_AES_OUT_PTR_OFFSET,
        .words_per_frame = SGI_RAD1_AUDIO_AES_WORDS,
        .freq = SGI_RAD1_AUDIO_FREQ,
        .iface = SGI_RAD1_AUDIO_IFACE_AES,
    },
    [SGI_RAD1_AUDIO_CH_ADAT_IN] = {
        .name = "ADAT input",
        .voice = "sgi-rad1-audio.adat-in",
        .output = false,
        .tag = SGI_RAD1_AUDIO_CH_ADAT_IN,
        .addr_lo = SGI_RAD1_AUDIO_REG_ADAT_IN_A_ADDR_LO,
        .addr_hi = SGI_RAD1_AUDIO_REG_ADAT_IN_A_ADDR_HI,
        .ctrl = SGI_RAD1_AUDIO_REG_ADAT_IN_A_CTRL,
        .enable_bit = SGI_RAD1_AUDIO_ENABLE_BIT,
        .pos_a = SGI_RAD1_AUDIO_REG_ADAT_IN_POS_A,
        .pos_a_shift = SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT,
        .pos_b = SGI_RAD1_AUDIO_REG_ADAT_IN_POS_B,
        .pos_b_shift = SGI_RAD1_AUDIO_DIGITAL_POS_B_SHIFT,
        .slot_ptr = SGI_RAD1_AUDIO_SLOT_ADAT_IN_PTR_OFFSET,
        .words_per_frame = SGI_RAD1_AUDIO_ADAT_WORDS,
        .freq = SGI_RAD1_AUDIO_ADAT_IN_FREQ,
        .iface = SGI_RAD1_AUDIO_IFACE_ADAT,
    },
    [SGI_RAD1_AUDIO_CH_ADAT_OUT] = {
        .name = "ADAT output",
        .voice = "sgi-rad1-audio.adat-out",
        .output = true,
        .tag = SGI_RAD1_AUDIO_CH_ADAT_OUT,
        .addr_lo = SGI_RAD1_AUDIO_REG_ADAT_OUT_A_ADDR_LO,
        .addr_hi = SGI_RAD1_AUDIO_REG_ADAT_OUT_A_ADDR_HI,
        .ctrl = SGI_RAD1_AUDIO_REG_ADAT_OUT_A_CTRL,
        .enable_bit = SGI_RAD1_AUDIO_ENABLE_BIT,
        .pos_a = SGI_RAD1_AUDIO_REG_ADAT_OUT_POS_A,
        .pos_a_shift = SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT,
        .pos_b = SGI_RAD1_AUDIO_REG_ADAT_OUT_POS_B,
        .pos_b_shift = SGI_RAD1_AUDIO_DIGITAL_POS_B_SHIFT,
        .slot_ptr = SGI_RAD1_AUDIO_SLOT_ADAT_OUT_PTR_OFFSET,
        .words_per_frame = SGI_RAD1_AUDIO_ADAT_WORDS,
        .freq = SGI_RAD1_AUDIO_FREQ,
        .iface = SGI_RAD1_AUDIO_IFACE_ADAT,
    },
    [SGI_RAD1_AUDIO_CH_AES_SUB] = {
        .name = "AES3-input subcode",
        .voice = "sgi-rad1-audio.aes-sub",
        .output = false,
        .tag = SGI_RAD1_AUDIO_CH_AES_SUB,
        .addr_lo = SGI_RAD1_AUDIO_REG_AES_SUB_A_ADDR_LO,
        .addr_hi = SGI_RAD1_AUDIO_REG_AES_SUB_A_ADDR_HI,
        .ctrl = SGI_RAD1_AUDIO_REG_AES_SUB_A_CTRL,
        .enable_bit = SGI_RAD1_AUDIO_SUBCODE_ENABLE_BIT,
        .pos_a = SGI_RAD1_AUDIO_REG_AES_IN_POS_B,
        .pos_a_shift = SGI_RAD1_AUDIO_SUB_POS_A_SHIFT,
        .pos_b = SGI_RAD1_AUDIO_REG_AES_IN_POS_B,
        .pos_b_shift = SGI_RAD1_AUDIO_DIGITAL_POS_B_SHIFT,
        .pos_b_parent = true,
        .slot_ptr = SGI_RAD1_AUDIO_SLOT_AES_SUB_PTR_OFFSET,
        .words_per_frame = SGI_RAD1_AUDIO_AES_SUB_WORDS,
        .freq = 0,
        .iface = SGI_RAD1_AUDIO_IFACE_NONE,
    },
    [SGI_RAD1_AUDIO_CH_ADAT_SUB] = {
        .name = "ADAT-input subcode",
        .voice = "sgi-rad1-audio.adat-sub",
        .output = false,
        .tag = SGI_RAD1_AUDIO_CH_ADAT_SUB,
        .addr_lo = SGI_RAD1_AUDIO_REG_ADAT_SUB_A_ADDR_LO,
        .addr_hi = SGI_RAD1_AUDIO_REG_ADAT_SUB_A_ADDR_HI,
        .ctrl = SGI_RAD1_AUDIO_REG_ADAT_SUB_A_CTRL,
        .enable_bit = SGI_RAD1_AUDIO_SUBCODE_ENABLE_BIT,
        .pos_a = SGI_RAD1_AUDIO_REG_ADAT_IN_POS_B,
        .pos_a_shift = SGI_RAD1_AUDIO_SUB_POS_A_SHIFT,
        .pos_b = SGI_RAD1_AUDIO_REG_ADAT_IN_POS_B,
        .pos_b_shift = SGI_RAD1_AUDIO_DIGITAL_POS_B_SHIFT,
        .pos_b_parent = true,
        .slot_ptr = SGI_RAD1_AUDIO_SLOT_ADAT_SUB_PTR_OFFSET,
        .words_per_frame = SGI_RAD1_AUDIO_ADAT_SUB_WORDS,
        .freq = SGI_RAD1_AUDIO_ADAT_IN_FREQ,
        .iface = SGI_RAD1_AUDIO_IFACE_NONE,
    },
};

/*
 * The completion word for the connected AES3 source: done with bit 19 set
 * when nothing is connected, else done with
 * the documented rate code for a locked input. The property is checked at
 * realize.
 */
static uint32_t sgi_rad1_audio_done_word(SGIRad1AudioState *s)
{
    uint32_t code;

    switch (s->aes_input_rate) {
    case 48000:
        code = SGI_RAD1_AUDIO_AES_CODE_48000;
        break;
    case 44100:
        code = SGI_RAD1_AUDIO_AES_CODE_44100;
        break;
    case 32000:
        code = SGI_RAD1_AUDIO_AES_CODE_32000;
        break;
    default:
        return SGI_RAD1_AUDIO_SLOT_DONE_WORD;
    }
    return SGI_RAD1_AUDIO_SLOT_DONE_BIT |
           (code << SGI_RAD1_AUDIO_SLOT_AES_RATE_SHIFT);
}

/* The field-event bit for video field @n: even and odd alternate. */
static uint32_t sgi_rad1_audio_field_bit(uint64_t n)
{
    return (n & 1) ? SGI_RAD1_AUDIO_SLOT_FIELD_ODD
                   : SGI_RAD1_AUDIO_SLOT_FIELD_EVEN;
}

/*
 * The field events for a completion now: one bit for each video field that
 * began since the last completion reported
 * one, at most the two a slot can carry. None with no reference connected.
 * See the header.
 */
static uint32_t sgi_rad1_audio_field_events(SGIRad1AudioState *s)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->ust_base_ns;
    uint64_t fields;
    uint32_t bits = 0;

    switch (s->genlock_mode) {
    case SGI_RAD1_AUDIO_GENLOCK_NTSC:
        fields = elapsed > 0 ? muldiv64(elapsed, SGI_RAD1_AUDIO_NTSC_FIELDS,
                                        SGI_RAD1_AUDIO_NTSC_NS) : 0;
        break;
    case SGI_RAD1_AUDIO_GENLOCK_PAL:
        fields = elapsed > 0 ? muldiv64(elapsed, SGI_RAD1_AUDIO_PAL_FIELDS,
                                        SGI_RAD1_AUDIO_PAL_NS) : 0;
        break;
    default:
        return 0;
    }
    if (fields > s->genlock_fields) {
        bits = sgi_rad1_audio_field_bit(fields);
        if (fields - s->genlock_fields > 1) {
            bits |= sgi_rad1_audio_field_bit(fields - 1);
        }
        trace_sgi_rad1_audio_field_events(fields, bits);
        s->genlock_fields = fields;
    }
    return bits;
}

/*
 * The flag word: ADAT present when the host says a source is connected, and
 * the video field events of a connected
 * reference.
 */
static uint32_t sgi_rad1_audio_flags_word(SGIRad1AudioState *s)
{
    return SGI_RAD1_AUDIO_SLOT_FLAGS_WORD |
           (s->adat_input ? SGI_RAD1_AUDIO_SLOT_ADAT_PRESENT : 0) |
           sgi_rad1_audio_field_events(s);
}

static void sgi_rad1_audio_sync_all(SGIRad1AudioState *s);

/* A channel's current DMA pointer, low 32 bits of the PCI address. */
static uint32_t sgi_rad1_audio_chan_pointer(SGIRad1AudioChannel *c)
{
    return (uint32_t)(c->base + c->offset);
}

/*
 * Fill one descriptor-ring slot, as a completion of the status DMA.
 *
 * - +0x44..+0x74: registers 0x04..0x34, the position bank, the 0x30 counter
 * and the sync status, as they read now.
 *   The channel engines are brought up to the present first, so the positions
 * and pointers are those of this
 *   completion, not of an engine's last timer step.
 * - the even words of +0x00..+0x38: each channel's DMA pointer, once the
 * channel has been started since reset. The
 *   odd words are left alone: the driver does not read them.
 * - +0x78 and +0x7c: the digital receivers and the video reference as the
 * host describes them, and done. The done
 *   word goes last, so the driver never sees a done slot with a stale body.
 *
 * Every word goes through the device's own PCI DMA address space, written
 * little-endian. The RAD1 is a
 * little-endian PCI device that is not byte-lane self-ordered, so the Bridge
 * byte-reverses its DMA into the
 * big-endian host memory the driver reads with plain loads; writing
 * big-endian here would put the done bit in the
 * low byte.
 */
static bool sgi_rad1_audio_fill_slot(SGIRad1AudioState *s, uint64_t slot)
{
    PCIDevice *dev = PCI_DEVICE(s);
    uint8_t mirror[SGI_RAD1_AUDIO_SLOT_MIRROR_WORDS * 4];
    uint8_t word[4];
    uint32_t output_ptr = 0;
    unsigned int i;

    sgi_rad1_audio_sync_all(s);

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

    for (i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        SGIRad1AudioChannel *c = &s->chan[i];

        if (c->len || s->octane) {
            stl_le_p(word, c->len ? sgi_rad1_audio_chan_pointer(c) : 0);
            if (i == SGI_RAD1_AUDIO_CH_OUT) {
                output_ptr = ldl_le_p(word);
            }
            if (pci_dma_write(dev,
                              slot + sgi_rad1_audio_channel_info[i].slot_ptr,
                              word, sizeof(word)) != MEMTX_OK && s->octane) {
                goto failed;
            }
        }
    }

    stl_le_p(word, sgi_rad1_audio_flags_word(s));
    if (pci_dma_write(dev, slot + SGI_RAD1_AUDIO_SLOT_FLAGS_OFFSET,
                      word, sizeof(word)) != MEMTX_OK && s->octane) {
        goto failed;
    }
    stl_le_p(word, sgi_rad1_audio_done_word(s));
    if (pci_dma_write(dev, slot + SGI_RAD1_AUDIO_SLOT_DONE_OFFSET,
                      word, sizeof(word)) != MEMTX_OK && s->octane) {
        goto failed;
    }
    if (s->octane) {
        SGIRad1AudioChannel *c = &s->chan[SGI_RAD1_AUDIO_CH_OUT];

        trace_sgi_rad1_octane_status_snapshot(slot,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), c->start_ns, c->frames,
            output_ptr,
            ldl_le_p(mirror + SGI_RAD1_AUDIO_REG_OUT_POS_A -
                     SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST),
            ldl_le_p(mirror + SGI_RAD1_AUDIO_REG_ANALOG_POS_B -
                     SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST),
            ldl_le_p(mirror + SGI_RAD1_AUDIO_REG_UST_COUNTER -
                     SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST), s->status_running);
    }
    return true;

failed:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-rad1-octane: status DMA at 0x%" PRIx64 " failed\n",
                  slot);
    return false;
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
 * rad_start_status_dma encodes it as LIMIT = 0x78 - count
 * * 0x80. A limit that does not decode to a whole ring falls back to a single
 * slot, so a caller that programmed the
 * ring differently than the driver still sees a completion.
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
 * -4 limit, writes 0x40001 to 0x5c, and
 * busy-polls the base slot's +0x7c bit 31. The engine fills that one slot,
 * exactly as a continuous completion
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
 * Continuous-engine tick: fill the next slot and re-arm for the next deadline
 * on the absolute schedule. The cursor
 * advances in ring order with wrap: the driver's servicer consumes
 * consecutive done slots from just past its own
 * consumer index and clears each done bit, so filling the next slot each
 * completion keeps it finding fresh ones.
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
 * to 0 and starts producing; the model
 * starts its cursor one past that (the first slot the servicer checks) and
 * schedules the first completion one
 * interval out. A zero interval names no period, so it stops the engine
 * rather than completing slots as fast as the
 * host can; an interval below the floor is raised to it (see the header).
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
    if (!s->octane && interval < SGI_RAD1_AUDIO_STATUS_INTERVAL_MIN) {
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
 * The backend calls these when a voice has room or has samples. The engines
 * move frames on their own timers
 * instead, so nothing happens here.
 */
static void sgi_rad1_audio_out_callback(void *opaque, int avail)
{
}

static void sgi_rad1_audio_in_callback(void *opaque, int avail)
{
}

static unsigned int sgi_rad1_audio_chan_index(SGIRad1AudioChannel *c)
{
    return c - c->dev->chan;
}

static const SGIRad1AudioChannelInfo *
sgi_rad1_audio_chan_info(SGIRad1AudioChannel *c)
{
    return &sgi_rad1_audio_channel_info[sgi_rad1_audio_chan_index(c)];
}

/* A channel's sample rate; see the header for why each is what it is. */
static uint32_t sgi_rad1_audio_chan_freq(SGIRad1AudioChannel *c)
{
    const SGIRad1AudioChannelInfo *ci = sgi_rad1_audio_chan_info(c);

    if (ci->freq) {
        return ci->freq;
    }
    return c->dev->aes_input_rate ?: SGI_RAD1_AUDIO_AES_DEFAULT_FREQ;
}

static unsigned int sgi_rad1_audio_frame_bytes(SGIRad1AudioChannel *c)
{
    return sgi_rad1_audio_chan_info(c)->words_per_frame *
           SGI_RAD1_AUDIO_WORD_BYTES;
}

/* The backend a channel's voice lives on, or NULL for none. */
static AudioBackend *sgi_rad1_audio_chan_be(SGIRad1AudioChannel *c)
{
    switch (sgi_rad1_audio_chan_info(c)->iface) {
    case SGI_RAD1_AUDIO_IFACE_AES:
        return c->dev->aes_be;
    case SGI_RAD1_AUDIO_IFACE_ADAT:
        return c->dev->adat_be;
    case SGI_RAD1_AUDIO_IFACE_NONE:
        return NULL;
    default:
        return c->dev->audio_be;
    }
}

/* The voice format: stereo 16-bit little-endian at the channel's rate. */
static struct audsettings sgi_rad1_audio_chan_settings(SGIRad1AudioChannel *c)
{
    return (struct audsettings) {
        .freq = sgi_rad1_audio_chan_freq(c),
        .nchannels = SGI_RAD1_AUDIO_HOST_CHANNELS,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };
}

/* An output channel's voice, opened at realize when it has a backend. */
static void sgi_rad1_audio_open_out(SGIRad1AudioChannel *c)
{
    AudioBackend *be = sgi_rad1_audio_chan_be(c);
    struct audsettings as = sgi_rad1_audio_chan_settings(c);

    if (be) {
        c->out_voice = audio_be_open_out(be, NULL,
                                         sgi_rad1_audio_chan_info(c)->voice,
                                         c, sgi_rad1_audio_out_callback, &as);
    }
}

/*
 * An input voice opens the first time its channel runs and then stays open
 * for the device's lifetime, like an
 * output voice. Opening it at realize would make a backend with no input,
 * such as wav, report an error on every
 * boot whether or not the guest ever records; this way it reports once, when
 * a recording starts.
 */
static void sgi_rad1_audio_open_in(SGIRad1AudioChannel *c)
{
    AudioBackend *be = sgi_rad1_audio_chan_be(c);
    struct audsettings as = sgi_rad1_audio_chan_settings(c);

    if (!be || c->in_voice || c->in_open_tried) {
        return;
    }
    c->in_open_tried = true;
    c->in_voice = audio_be_open_in(be, NULL,
                                   sgi_rad1_audio_chan_info(c)->voice, c,
                                   sgi_rad1_audio_in_callback, &as);
}

static void sgi_rad1_audio_set_voice(SGIRad1AudioChannel *c, bool on)
{
    AudioBackend *be = sgi_rad1_audio_chan_be(c);

    if (!be) {
        return;
    }
    if (sgi_rad1_audio_chan_info(c)->output) {
        if (c->out_voice) {
            audio_be_set_active_out(be, c->out_voice, on);
        }
        return;
    }
    if (on) {
        sgi_rad1_audio_open_in(c);
    }
    if (c->in_voice) {
        audio_be_set_active_in(be, c->in_voice, on);
    }
}

/*
 * Credit frames to a channel's position registers. Both count frames, not
 * words: the documented default.
 *
 * A: the frame count in bits [31:18], with natural 32-bit wrap, and the 0x30
 * counter mod 2^18 latched into bits
 * [17:0] as the count moves. rad_sync_device_ust splices those low bits into
 * its UST.
 *
 * B: a 14-bit count at the channel's shift, 4 for the analog output and 18
 * for every other type. rad_start_dma's
 * post-enable poll waits for it to change. Only that field moves: the rest of
 * each B register belongs to another
 * channel or to subcode (see the header).
 */
/* Add @frames to the 14-bit count at @shift in *@reg, wrapping in place. */
static void sgi_rad1_audio_add_field(uint32_t *reg, unsigned int shift,
                                     uint32_t frames)
{
    uint32_t mask = SGI_RAD1_AUDIO_POS_FIELD_MASK << shift;
    uint32_t field = ((*reg >> shift) + frames) & SGI_RAD1_AUDIO_POS_FIELD_MASK;

    *reg = (*reg & ~mask) | (field << shift);
}

static void sgi_rad1_audio_advance_pos(SGIRad1AudioChannel *c,
                                       uint32_t frames)
{
    SGIRad1AudioState *s = c->dev;
    const SGIRad1AudioChannelInfo *ci = sgi_rad1_audio_chan_info(c);
    uint32_t *pos_a = &s->sample_pos[SGI_RAD1_AUDIO_POS_IDX(ci->pos_a)];
    uint32_t *pos_b = &s->sample_pos[SGI_RAD1_AUDIO_POS_IDX(ci->pos_b)];

    sgi_rad1_audio_add_field(pos_a, ci->pos_a_shift, frames);
    if (ci->pos_a_shift == SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT) {
        uint32_t ust;

        if (s->octane) {
            int64_t boundary = c->start_ns +
                muldiv64_round_up(c->frames, NANOSECONDS_PER_SECOND,
                                  sgi_rad1_audio_chan_freq(c));

            ust = sgi_rad1_octane_ust_at(s, boundary);
        } else {
            ust = sgi_rad1_audio_ust_counter(s);
        }
        *pos_a = (*pos_a & ~SGI_RAD1_AUDIO_UST_LATCH_MASK) |
                 (ust & SGI_RAD1_AUDIO_UST_LATCH_MASK);
    }
    if (!ci->pos_b_parent) {
        sgi_rad1_audio_add_field(pos_b, ci->pos_b_shift, frames);
    }

    trace_sgi_rad1_audio_position(sgi_rad1_audio_chan_index(c), frames,
                                  *pos_a, *pos_b);
}

static void sgi_rad1_audio_chan_stop(SGIRad1AudioChannel *c,
                                     const char *reason)
{
    if (c->timer) {
        timer_del(c->timer);
    }
    if (!c->running) {
        return;
    }
    c->running = false;
    sgi_rad1_audio_set_voice(c, false);
    trace_sgi_rad1_audio_stop(sgi_rad1_audio_chan_index(c),
                              sgi_rad1_audio_chan_pointer(c), c->frames);
    trace_sgi_rad1_audio_stop_reason(sgi_rad1_audio_chan_index(c), reason,
                                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                     c->start_ns, c->frames,
                                     sgi_rad1_audio_chan_pointer(c));
}

/*
 * One step of an output: read the ring frames at the pointer and push them to
 * the voice as stereo 16-bit frames
 * (sgi-rad1-audio-pcm.h). The read happens whatever the backend accepts, so
 * the pointer and positions advance the
 * same with or without an audio device; a backend that takes less drops the
 * rest.
 */
static bool sgi_rad1_audio_out_chunk(SGIRad1AudioChannel *c, uint8_t *ring,
                                     size_t frames)
{
    AudioBackend *be = sgi_rad1_audio_chan_be(c);
    uint8_t pcm[SGI_RAD1_AUDIO_CHUNK_BYTES / 2];

    if (pci_dma_read(PCI_DEVICE(c->dev), c->base + c->offset, ring,
                     frames * sgi_rad1_audio_frame_bytes(c)) != MEMTX_OK) {
        return false;
    }
    if (be && c->out_voice) {
        sgi_rad1_audio_frames_to_pcm(ring, pcm, frames,
                                     sgi_rad1_audio_chan_info(c)->
                                     words_per_frame);
        audio_be_write(be, c->out_voice, pcm,
                       frames * SGI_RAD1_AUDIO_HOST_CHANNELS * 2);
    }
    return true;
}

/*
 * One step of an input: take stereo frames from the voice and write them into
 * the ring at the pointer
 * (sgi-rad1-audio-pcm.h). Whatever the voice does not supply is silence, so
 * the pointer and positions advance at
 * the sample rate with or without an audio device.
 */
static bool sgi_rad1_audio_in_chunk(SGIRad1AudioChannel *c, uint8_t *ring,
                                    size_t frames)
{
    AudioBackend *be = sgi_rad1_audio_chan_be(c);
    uint8_t pcm[SGI_RAD1_AUDIO_CHUNK_BYTES / 2];
    size_t want = frames * SGI_RAD1_AUDIO_HOST_CHANNELS * 2;
    size_t got = 0;

    if (be && c->in_voice) {
        got = audio_be_read(be, c->in_voice, pcm, want);
    }
    memset(pcm + got, 0, want - got);
    sgi_rad1_audio_pcm_to_frames(pcm, ring, frames,
                                 sgi_rad1_audio_chan_info(c)->words_per_frame);
    return pci_dma_write(PCI_DEVICE(c->dev), c->base + c->offset, ring,
                         frames * sgi_rad1_audio_frame_bytes(c)) == MEMTX_OK;
}

/*
 * Move frames between the ring and the voice, wrapping at the ring's end. The
 * ring is reached through the device's
 * PCI DMA address space, so the Bridge's translation and byte-lane reversal
 * apply.
 */
static bool sgi_rad1_audio_chan_move(SGIRad1AudioChannel *c, uint64_t frames)
{
    const SGIRad1AudioChannelInfo *ci = sgi_rad1_audio_chan_info(c);
    unsigned int frame_bytes = sgi_rad1_audio_frame_bytes(c);
    uint8_t ring[SGI_RAD1_AUDIO_CHUNK_BYTES];

    while (frames) {
        uint64_t bytes = MIN(frames * frame_bytes, c->len - c->offset);
        size_t chunk = MIN(bytes, sizeof(ring));
        size_t n = chunk / frame_bytes;
        bool ok = ci->output ? sgi_rad1_audio_out_chunk(c, ring, n)
                             : sgi_rad1_audio_in_chunk(c, ring, n);

        if (!ok) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-rad1-audio: %s DMA at 0x%" PRIx64
                          " failed; ring address unmapped?\n",
                          ci->name, c->base + c->offset);
            return false;
        }
        c->offset += chunk;
        if (c->dev->octane) {
            c->frames += n;
            sgi_rad1_audio_advance_pos(c, n);
        }
        frames -= n;
        if (c->offset >= c->len) {
            c->offset = 0;
            trace_sgi_rad1_audio_wrap(sgi_rad1_audio_chan_index(c), c->base);
        }
    }
    return true;
}

/*
 * Bring a channel up to the present. The frames due are those the sample
 * clock has produced since the enable edge;
 * computing them from elapsed virtual time keeps the rate exact however late
 * a timer fires. A status completion and
 * a position-register read call this too, so each sees the pointer and
 * positions as of that instant.
 */
static void sgi_rad1_audio_chan_sync(SGIRad1AudioChannel *c)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t due, frames;

    /* Elapsed time at or below zero would become a huge unsigned count. */
    if (!c->running || now <= c->start_ns) {
        return;
    }

    due = muldiv64(now - c->start_ns, sgi_rad1_audio_chan_freq(c),
                   NANOSECONDS_PER_SECOND);
    if (due > c->frames) {
        frames = due - c->frames;
        if (!sgi_rad1_audio_chan_move(c, frames)) {
            sgi_rad1_audio_chan_stop(c, "dma-failure");
            return;
        }
        if (!c->dev->octane) {
            c->frames = due;
            sgi_rad1_audio_advance_pos(c, (uint32_t)frames);
        }
    }
}

static void sgi_rad1_audio_sync_all(SGIRad1AudioState *s)
{
    unsigned int i;

    for (i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        sgi_rad1_audio_chan_sync(&s->chan[i]);
    }
}

/* Arm a channel's timer TICK_FRAMES frames past what it has moved. */
static void sgi_rad1_audio_chan_arm(SGIRad1AudioChannel *c)
{
    /* Round up, so the frames due when it fires are never one short. */
    timer_mod(c->timer, c->start_ns +
              muldiv64_round_up(c->frames + SGI_RAD1_AUDIO_TICK_FRAMES,
                                NANOSECONDS_PER_SECOND,
                                sgi_rad1_audio_chan_freq(c)));
}

/* Engine step: catch up, then re-arm. */
static void sgi_rad1_audio_chan_tick(void *opaque)
{
    SGIRad1AudioChannel *c = opaque;

    sgi_rad1_audio_chan_sync(c);
    if (c->running) {
        sgi_rad1_audio_chan_arm(c);
    }
}

/*
 * The enable edge. rad_start_dma programs set A and set B with the same ring
 * and control word, then sets bit 0 of
 * the channel's enable register; that write alone starts the channel. The
 * engine latches set A.
 */
static void sgi_rad1_audio_chan_start(SGIRad1AudioChannel *c)
{
    SGIRad1AudioState *s = c->dev;
    const SGIRad1AudioChannelInfo *ci = sgi_rad1_audio_chan_info(c);
    uint32_t addr_lo = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(ci->addr_lo)];
    uint32_t addr_hi = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(ci->addr_hi)];
    uint32_t ctrl = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(ci->ctrl)];
    uint64_t base = ((uint64_t)addr_hi << 32) | addr_lo;
    uint32_t neg = (ctrl >> SGI_RAD1_AUDIO_CTRL_LEN_SHIFT) &
                   SGI_RAD1_AUDIO_CTRL_LEN_MASK;
    uint32_t units = (0u - neg) & SGI_RAD1_AUDIO_CTRL_LEN_MASK;
    uint64_t len = (uint64_t)units * SGI_RAD1_AUDIO_DMA_UNIT_BYTES;
    uint32_t tag = (ctrl >> SGI_RAD1_AUDIO_CTRL_TAG_SHIFT) &
                   SGI_RAD1_AUDIO_CTRL_TAG_MASK;

    if (base == 0 || len == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: %s enabled with no ring programmed "
                      "(address 0x%" PRIx64 ", control 0x%x)\n",
                      ci->name, base, ctrl);
        return;
    }
    if (len > SGI_RAD1_AUDIO_DMA_MAX_BYTES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: %s ring length 0x%" PRIx64
                      " exceeds the modeled cap; clamping\n", ci->name, len);
        len = SGI_RAD1_AUDIO_DMA_MAX_BYTES;
    }
    /*
     * The length comes in 128-byte units, which a 256-byte subcode frame need
     * not divide. What hardware does with a
     * partial frame is unknown; the model starts nothing. The driver's rings
     * are whole frames.
     */
    if (len % sgi_rad1_audio_frame_bytes(c)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: %s ring length 0x%" PRIx64
                      " is not a whole number of %u-byte frames\n",
                      ci->name, len, sgi_rad1_audio_frame_bytes(c));
        return;
    }
    if (tag != ci->tag) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-rad1-audio: %s control word 0x%x carries tag %u, "
                      "not the channel type %u\n",
                      ci->name, ctrl, tag, ci->tag);
    }

    c->base = base;
    c->len = len;
    c->offset = 0;
    c->frames = 0;
    c->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    c->running = true;
    sgi_rad1_audio_set_voice(c, true);
    trace_sgi_rad1_audio_start(sgi_rad1_audio_chan_index(c), base, c->len,
                               tag);
    sgi_rad1_audio_chan_arm(c);
}

/*
 * A write to a channel's enable register. The channel's own bit, bit 0 or for
 * an input subcode channel bit 2,
 * starts and stops it; the register's other bits never do. Rewriting a set
 * bit while the engine runs does not
 * restart it.
 */
static void sgi_rad1_audio_enable_write(SGIRad1AudioChannel *c, uint32_t val)
{
    if (val & sgi_rad1_audio_chan_info(c)->enable_bit) {
        if (!c->running) {
            sgi_rad1_audio_chan_start(c);
        }
    } else {
        /* Stop where the sample clock has got to, not the last step. */
        sgi_rad1_audio_chan_sync(c);
        sgi_rad1_audio_chan_stop(c, "disable");
    }
}

/*
 * BAR0: the documented register file. None of these offsets has an
 * independently confirmed reset value or read
 * behavior, so reads return the guest's last write. This is a register-store
 * approximation, not verified hardware behavior. An offset this model does
 * not recognize logs as
 * unimplemented rather than accepting a
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
         * until a channel's engine advances it; only
         * the analog channels' registers (0x14, 0x18, 0x2c) move. Read-only,
         * so a guest write never changes it. The
         * engines catch up first, so a read agrees with a status slot filled
         * at the same instant.
         */
        sgi_rad1_audio_sync_all(s);
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
    /*
     * The digital channels' enables, bit 0 of each, and the input subcode
     * channels', bit 2 of 0x44 and 0x48; the
     * other bits (output subcode controls, bring-up values) are shadowed and
     * start nothing.
     */
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_A:
        s->predispatch_a = val;
        sgi_rad1_audio_enable_write(&s->chan[SGI_RAD1_AUDIO_CH_ADAT_IN], val);
        sgi_rad1_audio_enable_write(&s->chan[SGI_RAD1_AUDIO_CH_ADAT_SUB], val);
        break;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_B:
        s->predispatch_b = val;
        sgi_rad1_audio_enable_write(&s->chan[SGI_RAD1_AUDIO_CH_AES_IN], val);
        sgi_rad1_audio_enable_write(&s->chan[SGI_RAD1_AUDIO_CH_AES_SUB], val);
        break;
    case SGI_RAD1_AUDIO_REG_SHIFT_LOAD_CLOCK:
        /*
         * Bit 0 starts and stops the analog input. Bring-up writes 0x5000000
         * with it clear, and the MIO mux
         * bit-bang pulses bit 27 while the driver's shadow keeps bit 0, so
         * those writes leave the input as it was.
         */
        /* Bit 27 clear to set clocks the next mux word bit in from 0x8c. */
        if (!(s->shift_load_clock & SGI_RAD1_AUDIO_MUX_CLOCK) &&
            (val & SGI_RAD1_AUDIO_MUX_CLOCK)) {
            s->mux_shift = (s->mux_shift << 1) |
                           (s->shift_load_data & SGI_RAD1_AUDIO_MUX_DATA);
        }
        s->shift_load_clock = val;
        sgi_rad1_audio_enable_write(&s->chan[SGI_RAD1_AUDIO_CH_IN], val);
        break;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_C:
        s->predispatch_c = val;
        sgi_rad1_audio_enable_write(&s->chan[SGI_RAD1_AUDIO_CH_ADAT_OUT], val);
        break;
    case SGI_RAD1_AUDIO_REG_PREDISPATCH_D:
        s->predispatch_d = val;
        sgi_rad1_audio_enable_write(&s->chan[SGI_RAD1_AUDIO_CH_AES_OUT], val);
        break;
    case SGI_RAD1_AUDIO_REG_ANALOG_OUT_EN:
        /*
         * Bit 0 starts and stops the analog output. Bring-up writes
         * 0x20000000 with it clear; rad_start_dma sets it
         * and rad_stop_dma clears it, keeping bit 29 from the shadow.
         */
        s->analog_out_en = val;
        sgi_rad1_audio_enable_write(&s->chan[SGI_RAD1_AUDIO_CH_OUT], val);
        break;
    case SGI_RAD1_AUDIO_REG_STATUS_DMA_CTRL:
        s->status_dma_ctrl = val;
        /*
         * The ring bit (0x2) tells the continuous ring engine (0x40003) from
         * the one-shot (0x40001); the interval
         * field in bits [16:2] is masked off first. The continuous start arms
         * the periodic completion timer; the
         * one-shot marks a single slot now; the stop word (0x1fffc) halts the
         * timer; any other write only records
         * the register.
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
        /* Central control: ADAT-receiver and clock-generator bits. No
         * channel's transfer starts or waits on it. */
        s->shadow_masked = val;
        break;
    case SGI_RAD1_AUDIO_REG_MODE_GATED_A:
        s->mode_gated_a = val;
        break;
    case SGI_RAD1_AUDIO_REG_LATCH_PULSE:
        /* 2 then 3 latches the mux word shifted in so far. */
        if (s->latch_pulse == SGI_RAD1_AUDIO_MUX_LATCH_ARM &&
            val == SGI_RAD1_AUDIO_MUX_LATCH_FIRE) {
            s->mux_word = s->mux_shift &
                          MAKE_64BIT_MASK(0, SGI_RAD1_AUDIO_MUX_BITS);
            trace_sgi_rad1_audio_mux_word(s->mux_word);
        }
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

/*
 * The output subcode windows, 0x1000..0x121f. The driver only stores here; a
 * read answers from the shadow like the
 * register file's other stores. The undocumented doubleword between the ADAT
 * and AES3 windows logs as
 * unimplemented.
 */
static bool sgi_rad1_audio_subcode_gap(hwaddr addr, unsigned size)
{
    hwaddr first = SGI_RAD1_AUDIO_SUBCODE_FIRST + addr;
    hwaddr last = first + size - 1;

    return last >= SGI_RAD1_AUDIO_SUBCODE_GAP_FIRST &&
           first <= SGI_RAD1_AUDIO_SUBCODE_GAP_LAST;
}

static uint64_t sgi_rad1_audio_subcode_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    SGIRad1AudioState *s = opaque;

    if (sgi_rad1_audio_subcode_gap(addr, size)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-rad1-audio: read of BAR0 offset 0x%" HWADDR_PRIx
                      " which this model does not implement\n",
                      SGI_RAD1_AUDIO_SUBCODE_FIRST + addr);
        return 0;
    }
    return ldn_le_p(s->subcode + addr, size);
}

static void sgi_rad1_audio_subcode_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    SGIRad1AudioState *s = opaque;

    if (sgi_rad1_audio_subcode_gap(addr, size)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-rad1-audio: write of 0x%" PRIx64
                      " to BAR0 offset 0x%" HWADDR_PRIx
                      " which this model does not implement\n",
                      val, SGI_RAD1_AUDIO_SUBCODE_FIRST + addr);
        return;
    }
    stn_le_p(s->subcode + addr, size, val);
}

static const MemoryRegionOps sgi_rad1_audio_subcode_ops = {
    .read = sgi_rad1_audio_subcode_read,
    .write = sgi_rad1_audio_subcode_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps sgi_rad1_audio_bar0_ops = {
    .read = sgi_rad1_audio_bar0_read,
    .write = sgi_rad1_audio_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* Selected Octane board policy; physical UST timing is unmeasured. */
#define SGI_RAD1_OCTANE_UST_NS_PER_TICK 960
#define SGI_RAD1_OCTANE_REG_UST_CONTROL 0x40

static void sgi_rad1_octane_ust_update(SGIRad1AudioState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->octane_ust_enabled && now > s->octane_ust_last_ns) {
        uint64_t elapsed = now - s->octane_ust_last_ns;
        uint64_t total = elapsed + s->octane_ust_phase;

        s->octane_ust_count += total / SGI_RAD1_OCTANE_UST_NS_PER_TICK;
        s->octane_ust_phase = total % SGI_RAD1_OCTANE_UST_NS_PER_TICK;
    }
    s->octane_ust_last_ns = now;
}

static uint32_t sgi_rad1_octane_ust_at(SGIRad1AudioState *s, int64_t ns)
{
    int64_t ticks = s->octane_ust_phase;

    if (s->octane_ust_enabled) {
        ticks += ns - s->octane_ust_last_ns;
    }
    return s->octane_ust_count +
           (ticks >= 0 ? ticks / SGI_RAD1_OCTANE_UST_NS_PER_TICK :
            -DIV_ROUND_UP(-ticks, SGI_RAD1_OCTANE_UST_NS_PER_TICK));
}

static bool sgi_rad1_octane_descriptor(hwaddr addr, bool *output)
{
    switch (addr) {
    case SGI_RAD1_AUDIO_REG_OUT_A_ADDR_LO:
    case SGI_RAD1_AUDIO_REG_OUT_A_ADDR_HI:
    case SGI_RAD1_AUDIO_REG_OUT_A_CTRL:
    case SGI_RAD1_AUDIO_REG_OUT_B_ADDR_LO:
    case SGI_RAD1_AUDIO_REG_OUT_B_ADDR_HI:
    case SGI_RAD1_AUDIO_REG_OUT_B_CTRL:
        *output = true;
        return true;
    case SGI_RAD1_AUDIO_REG_STATUS_BASE_LO:
    case SGI_RAD1_AUDIO_REG_STATUS_BASE_HI:
    case SGI_RAD1_AUDIO_REG_STATUS_LIMIT:
    case SGI_RAD1_AUDIO_REG_STATUS_BASE2_LO:
    case SGI_RAD1_AUDIO_REG_STATUS_BASE2_HI:
    case SGI_RAD1_AUDIO_REG_STATUS_LIMIT2:
        *output = false;
        return true;
    default:
        return false;
    }
}

static bool sgi_rad1_octane_ring_valid(SGIRad1AudioState *s, bool output,
                                      uint32_t limit)
{
    hwaddr lo = output ? SGI_RAD1_AUDIO_REG_OUT_A_ADDR_LO :
                        SGI_RAD1_AUDIO_REG_STATUS_BASE_LO;
    hwaddr hi = output ? SGI_RAD1_AUDIO_REG_OUT_A_ADDR_HI :
                        SGI_RAD1_AUDIO_REG_STATUS_BASE_HI;
    hwaddr ctrl = output ? SGI_RAD1_AUDIO_REG_OUT_A_CTRL :
                          SGI_RAD1_AUDIO_REG_STATUS_LIMIT;
    hwaddr lo2 = output ? SGI_RAD1_AUDIO_REG_OUT_B_ADDR_LO :
                         SGI_RAD1_AUDIO_REG_STATUS_BASE2_LO;
    hwaddr hi2 = output ? SGI_RAD1_AUDIO_REG_OUT_B_ADDR_HI :
                         SGI_RAD1_AUDIO_REG_STATUS_BASE2_HI;
    hwaddr ctrl2 = output ? SGI_RAD1_AUDIO_REG_OUT_B_CTRL :
                           SGI_RAD1_AUDIO_REG_STATUS_LIMIT2;
    uint32_t *bank = s->dma_bank;

    if (!(bank[SGI_RAD1_AUDIO_DMA_IDX(lo)] ||
          bank[SGI_RAD1_AUDIO_DMA_IDX(hi)]) ||
        bank[SGI_RAD1_AUDIO_DMA_IDX(ctrl)] != limit) {
        return false;
    }
    if (!output && limit == 0xfffffffc) {
        return true;
    }
    return bank[SGI_RAD1_AUDIO_DMA_IDX(lo)] ==
           bank[SGI_RAD1_AUDIO_DMA_IDX(lo2)] &&
           bank[SGI_RAD1_AUDIO_DMA_IDX(hi)] ==
           bank[SGI_RAD1_AUDIO_DMA_IDX(hi2)] &&
           bank[SGI_RAD1_AUDIO_DMA_IDX(ctrl2)] == limit;
}

static MemTxResult sgi_rad1_octane_read(void *opaque, hwaddr addr,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    SGIRad1AudioState *s = opaque;
    bool output;

    if (size == 4 && !(addr & 3)) {
        if (sgi_rad1_octane_descriptor(addr, &output)) {
            *value = s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(addr)];
            return MEMTX_OK;
        }
        if (addr >= SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST &&
            addr <= SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST) {
            sgi_rad1_audio_sync_all(s);
            *value = s->sample_pos[SGI_RAD1_AUDIO_POS_IDX(addr)];
            return MEMTX_OK;
        }
        if (addr == SGI_RAD1_AUDIO_REG_ANALOG_OUT_EN) {
            *value = s->analog_out_en;
            return MEMTX_OK;
        }
        if (addr == SGI_RAD1_AUDIO_REG_STATUS_DMA_CTRL) {
            *value = s->status_dma_ctrl;
            return MEMTX_OK;
        }
    }
    if (addr == SGI_RAD1_AUDIO_REG_UST_COUNTER && size == 4) {
        sgi_rad1_octane_ust_update(s);
        *value = s->octane_ust_count;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-rad1-octane: unsupported %u-byte BAR0 read at "
                  "0x%" HWADDR_PRIx "\n", size, addr);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult sgi_rad1_octane_write(void *opaque, hwaddr addr,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIRad1AudioState *s = opaque;
    bool output;

    if (size == 4 && !(addr & 3)) {
        if (sgi_rad1_octane_descriptor(addr, &output)) {
            if (!(output ? s->chan[SGI_RAD1_AUDIO_CH_OUT].running :
                           s->status_running)) {
                s->dma_bank[SGI_RAD1_AUDIO_DMA_IDX(addr)] = value;
                return MEMTX_OK;
            }
        } else if (addr == SGI_RAD1_AUDIO_REG_ANALOG_OUT_EN &&
                   (value == 0x20000000 || value == 0x20000001)) {
            if (value == 0x20000000 ||
                sgi_rad1_octane_ring_valid(s, true, 0xfffff008)) {
                SGIRad1AudioChannel *c = &s->chan[SGI_RAD1_AUDIO_CH_OUT];

                sgi_rad1_audio_enable_write(c, value);
                s->analog_out_en = value;
                trace_sgi_rad1_octane_output_control(value,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), c->start_ns,
                    c->frames, c->running);
                return MEMTX_OK;
            }
        } else if (addr == SGI_RAD1_AUDIO_REG_STATUS_DMA_CTRL) {
            if (value == SGI_RAD1_AUDIO_STATUS_STOP) {
                sgi_rad1_audio_status_ring_stop(s);
            } else if (value == SGI_RAD1_AUDIO_STATUS_ONESHOT_START &&
                       !s->status_running &&
                       sgi_rad1_octane_ring_valid(s, false, 0xfffffffc)) {
                sgi_rad1_audio_one_shot_status(s);
            } else if ((value & ~0x1fffcULL) == SGI_RAD1_AUDIO_STATUS_START &&
                       (value & 0x1fffc) && !s->status_running &&
                       sgi_rad1_octane_ring_valid(s, false, 0xffffc078)) {
                sgi_rad1_audio_status_ring_start(s, value);
            } else {
                goto unsupported;
            }
            s->status_dma_ctrl = value;
            trace_sgi_rad1_octane_status_control(value,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->status_running,
                s->status_start_ns, s->status_interval, s->status_ring_cursor);
            return MEMTX_OK;
        }
    }
    if (addr == SGI_RAD1_OCTANE_REG_UST_CONTROL && size == 4) {
        sgi_rad1_audio_sync_all(s);
        switch (value) {
        case 0x0e:
            sgi_rad1_octane_ust_update(s);
            s->octane_ust_enabled = false;
            s->octane_ust_armed = true;
            return MEMTX_OK;
        case 0x0a:
            if (s->octane_ust_armed) {
                sgi_rad1_octane_ust_update(s);
                s->octane_ust_enabled = true;
                s->octane_ust_armed = false;
                return MEMTX_OK;
            }
            break;
        case 0x02:
            sgi_rad1_octane_ust_update(s);
            s->octane_ust_enabled = false;
            s->octane_ust_armed = false;
            return MEMTX_OK;
        default:
            break;
        }
    }
unsupported:
    qemu_log_mask(LOG_UNIMP,
                  "sgi-rad1-octane: unsupported %u-byte BAR0 write at "
                  "0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n",
                  size, addr, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps sgi_rad1_octane_bar0_ops = {
    .read_with_attrs = sgi_rad1_octane_read,
    .write_with_attrs = sgi_rad1_octane_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static void sgi_rad1_audio_reset_hold(Object *obj, ResetType type)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(obj);
    unsigned int i;

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
    memset(s->subcode, 0, sizeof(s->subcode));

    /* Stop the channel engines and forget their rings; voices stay open. */
    for (i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        SGIRad1AudioChannel *c = &s->chan[i];

        sgi_rad1_audio_chan_stop(c, "reset");
        c->base = 0;
        c->len = 0;
        c->offset = 0;
        c->start_ns = 0;
        c->frames = 0;
    }

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
    s->genlock_fields = 0;
    s->mux_shift = 0;
    s->mux_word = 0;
}

static void sgi_rad1_audio_realize(PCIDevice *dev, Error **errp)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(dev);
    Error *audio_err = NULL;
    unsigned int i;

    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);

    /*
     * The MediaIO board always instantiates this device, so a missing audio
     * backend must not abort machine
     * creation. Resolve the audiodev property (or the default); if none is
     * available, warn and run without voices. The register file and DMA
     * position still advance; only host samples are skipped.
     */
    switch (s->aes_input_rate) {
    case 0:
    case 32000:
    case 44100:
    case 48000:
        break;
    default:
        error_setg(errp, "sgi-rad1-audio: aes-input-rate must be 0 (none), "
                   "32000, 44100 or 48000, not %u", s->aes_input_rate);
        return;
    }
    if (!s->genlock || !strcmp(s->genlock, "none")) {
        s->genlock_mode = SGI_RAD1_AUDIO_GENLOCK_NONE;
    } else if (!strcmp(s->genlock, "ntsc")) {
        s->genlock_mode = SGI_RAD1_AUDIO_GENLOCK_NTSC;
    } else if (!strcmp(s->genlock, "pal")) {
        s->genlock_mode = SGI_RAD1_AUDIO_GENLOCK_PAL;
    } else {
        error_setg(errp, "sgi-rad1-audio: genlock must be none, ntsc or pal, "
                   "not %s", s->genlock);
        return;
    }
    object_property_add_uint32_ptr(OBJECT(dev), "mux-word", &s->mux_word,
                                   OBJ_PROP_FLAG_READ);

    if (!audio_be_check(&s->audio_be, &audio_err)) {
        warn_report_err(audio_err);
        s->audio_be = NULL;
    }

    memory_region_init_io(&s->bar0, OBJECT(dev), &sgi_rad1_audio_bar0_ops, s,
                          "sgi-rad1-audio.bar0", SGI_RAD1_AUDIO_BAR0_SIZE);
    memory_region_init_io(&s->subcode_mr, OBJECT(dev),
                          &sgi_rad1_audio_subcode_ops, s,
                          "sgi-rad1-audio.subcode",
                          SGI_RAD1_AUDIO_SUBCODE_BYTES);
    memory_region_add_subregion(&s->bar0, SGI_RAD1_AUDIO_SUBCODE_FIRST,
                                &s->subcode_mr);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /* The continuous status-DMA completion timer; armed on a 0x40003
     * start. */
    s->status_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   sgi_rad1_audio_status_tick, s);
    /* Each channel's pacing timer; armed on its enable edge. */
    for (i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        SGIRad1AudioChannel *c = &s->chan[i];

        c->dev = s;
        c->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_rad1_audio_chan_tick,
                                c);
        if (sgi_rad1_audio_channel_info[i].output) {
            sgi_rad1_audio_open_out(c);
        }
    }
}

static void sgi_rad1_audio_exit(PCIDevice *dev)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(dev);
    unsigned int i;

    if (s->status_timer) {
        timer_free(s->status_timer);
        s->status_timer = NULL;
    }
    for (i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        SGIRad1AudioChannel *c = &s->chan[i];
        AudioBackend *be = sgi_rad1_audio_chan_be(c);

        if (c->timer) {
            timer_free(c->timer);
            c->timer = NULL;
        }
        if (be && c->out_voice) {
            audio_be_close_out(be, c->out_voice);
            c->out_voice = NULL;
        }
        if (be && c->in_voice) {
            audio_be_close_in(be, c->in_voice);
            c->in_voice = NULL;
        }
    }
}

/* Refuse a channel whose ring geometry would misdirect DMA. */
static bool sgi_rad1_audio_chan_valid(SGIRad1AudioChannel *c)
{
    unsigned int frame_bytes = sgi_rad1_audio_frame_bytes(c);

    return c->len <= SGI_RAD1_AUDIO_DMA_MAX_BYTES &&
           !(c->len % frame_bytes) &&
           !(c->offset % frame_bytes) &&
           (!c->len || c->offset < c->len) &&
           (!c->running || c->len);
}

/* The voices are not migrated; match their activity to the engines. */
static int sgi_rad1_audio_post_load(void *opaque, int version_id)
{
    SGIRad1AudioState *s = opaque;
    unsigned int i;

    for (i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        if (!sgi_rad1_audio_chan_valid(&s->chan[i])) {
            return -EINVAL;
        }
    }
    if (s->status_ring_slots > SGI_RAD1_AUDIO_SLOT_MAX ||
        (s->status_ring_slots &&
         s->status_ring_cursor >= s->status_ring_slots) ||
        (s->status_running &&
         (!s->status_interval || !s->status_ring_slots))) {
        return -EINVAL;
    }
    for (i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        sgi_rad1_audio_set_voice(&s->chan[i], s->chan[i].running);
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_rad1_audio_channel = {
    .name = "sgi-rad1-audio/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, SGIRad1AudioChannel),
        VMSTATE_BOOL(running, SGIRad1AudioChannel),
        VMSTATE_UINT64(base, SGIRad1AudioChannel),
        VMSTATE_UINT32(len, SGIRad1AudioChannel),
        VMSTATE_UINT32(offset, SGIRad1AudioChannel),
        VMSTATE_INT64(start_ns, SGIRad1AudioChannel),
        VMSTATE_UINT64(frames, SGIRad1AudioChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_rad1_audio = {
    .name = "sgi-rad1-audio",
    .version_id = 10,
    .minimum_version_id = 10,
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
        VMSTATE_UINT8_ARRAY(subcode, SGIRad1AudioState,
                            SGI_RAD1_AUDIO_SUBCODE_BYTES),
        VMSTATE_UINT32_ARRAY(sample_pos, SGIRad1AudioState,
                             SGI_RAD1_AUDIO_SAMPLE_POS_WORDS),
        VMSTATE_STRUCT_ARRAY(chan, SGIRad1AudioState, SGI_RAD1_AUDIO_NUM_CH,
                             1, vmstate_sgi_rad1_audio_channel,
                             SGIRad1AudioChannel),
        VMSTATE_TIMER_PTR(status_timer, SGIRad1AudioState),
        VMSTATE_UINT32(status_ring_cursor, SGIRad1AudioState),
        VMSTATE_UINT32(status_ring_slots, SGIRad1AudioState),
        VMSTATE_BOOL(status_running, SGIRad1AudioState),
        VMSTATE_UINT32(status_interval, SGIRad1AudioState),
        VMSTATE_INT64(status_start_ns, SGIRad1AudioState),
        VMSTATE_UINT64(status_done, SGIRad1AudioState),
        VMSTATE_INT64(ust_base_ns, SGIRad1AudioState),
        VMSTATE_UINT64(genlock_fields, SGIRad1AudioState),
        VMSTATE_UINT32(mux_shift, SGIRad1AudioState),
        VMSTATE_UINT32(mux_word, SGIRad1AudioState),
        VMSTATE_END_OF_LIST()
    }
};

static void sgi_rad1_octane_reset_hold(Object *obj, ResetType type)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(obj);

    sgi_rad1_audio_reset_hold(obj, type);
    s->octane_ust_count = 0;
    s->octane_ust_phase = 0;
    s->octane_ust_enabled = false;
    s->octane_ust_armed = false;
    s->octane_ust_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int sgi_rad1_octane_pre_save(void *opaque)
{
    SGIRad1AudioState *s = opaque;

    sgi_rad1_audio_sync_all(s);
    sgi_rad1_octane_ust_update(s);
    s->octane_migration_ns =
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return 0;
}

static int sgi_rad1_octane_post_load(void *opaque, int version_id)
{
    SGIRad1AudioState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t delta = now - s->octane_migration_ns;

    if (s->octane_ust_phase >= SGI_RAD1_OCTANE_UST_NS_PER_TICK ||
        sgi_rad1_audio_post_load(opaque, version_id)) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        SGIRad1AudioChannel *c = &s->chan[i];

        c->start_ns += delta;
        if (timer_pending(c->timer)) {
            timer_mod(c->timer, timer_expire_time_ns(c->timer) + delta);
        }
    }
    s->status_start_ns += delta;
    if (timer_pending(s->status_timer)) {
        timer_mod(s->status_timer,
                  timer_expire_time_ns(s->status_timer) + delta);
    }
    s->octane_ust_last_ns = now;
    return 0;
}

static const VMStateDescription vmstate_sgi_rad1_octane = {
    .name = "sgi-rad1-octane",
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_save = sgi_rad1_octane_pre_save,
    .post_load = sgi_rad1_octane_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIRad1AudioState),
        VMSTATE_UINT32_ARRAY(dma_bank, SGIRad1AudioState,
                             SGI_RAD1_AUDIO_DMA_BANK_WORDS),
        VMSTATE_UINT32_ARRAY(sample_pos, SGIRad1AudioState,
                             SGI_RAD1_AUDIO_SAMPLE_POS_WORDS),
        VMSTATE_UINT32(analog_out_en, SGIRad1AudioState),
        VMSTATE_UINT32(status_dma_ctrl, SGIRad1AudioState),
        VMSTATE_STRUCT_ARRAY(chan, SGIRad1AudioState, SGI_RAD1_AUDIO_NUM_CH,
                             1, vmstate_sgi_rad1_audio_channel,
                             SGIRad1AudioChannel),
        VMSTATE_TIMER_PTR(status_timer, SGIRad1AudioState),
        VMSTATE_UINT32(status_ring_cursor, SGIRad1AudioState),
        VMSTATE_UINT32(status_ring_slots, SGIRad1AudioState),
        VMSTATE_BOOL(status_running, SGIRad1AudioState),
        VMSTATE_UINT32(status_interval, SGIRad1AudioState),
        VMSTATE_INT64(status_start_ns, SGIRad1AudioState),
        VMSTATE_UINT64(status_done, SGIRad1AudioState),
        VMSTATE_INT64(octane_migration_ns, SGIRad1AudioState),
        VMSTATE_UINT32(octane_ust_count, SGIRad1AudioState),
        VMSTATE_UINT32(octane_ust_phase, SGIRad1AudioState),
        VMSTATE_BOOL(octane_ust_enabled, SGIRad1AudioState),
        VMSTATE_BOOL(octane_ust_armed, SGIRad1AudioState),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_rad1_octane_realize(PCIDevice *dev, Error **errp)
{
    SGIRad1AudioState *s = SGI_RAD1_AUDIO(dev);
    Error *audio_err = NULL;

    s->octane = true;
    if (s->aes_input_rate || s->adat_input ||
        (s->genlock && strcmp(s->genlock, "none"))) {
        error_setg(errp, "sgi-rad1-octane: digital input and genlock "
                   "unsupported");
        return;
    }
    if (!audio_be_check(&s->audio_be, &audio_err)) {
        warn_report_err(audio_err);
        s->audio_be = NULL;
    }
    s->status_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   sgi_rad1_audio_status_tick, s);
    for (unsigned i = 0; i < SGI_RAD1_AUDIO_NUM_CH; i++) {
        SGIRad1AudioChannel *c = &s->chan[i];

        c->dev = s;
        c->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                sgi_rad1_audio_chan_tick, c);
    }
    sgi_rad1_audio_open_out(&s->chan[SGI_RAD1_AUDIO_CH_OUT]);
    memory_region_init_io(&s->bar0, OBJECT(dev), &sgi_rad1_octane_bar0_ops,
                          s, "sgi-rad1-octane.bar0",
                          SGI_RAD1_AUDIO_BAR0_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
}

static const Property sgi_rad1_audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SGIRad1AudioState, audio_be),
    DEFINE_PROP_AUDIODEV("aes-audiodev", SGIRad1AudioState, aes_be),
    DEFINE_PROP_AUDIODEV("adat-audiodev", SGIRad1AudioState, adat_be),
    DEFINE_PROP_UINT32("aes-input-rate", SGIRad1AudioState, aes_input_rate, 0),
    DEFINE_PROP_BOOL("adat-input", SGIRad1AudioState, adat_input, false),
    DEFINE_PROP_STRING("genlock", SGIRad1AudioState, genlock),
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

static void sgi_rad1_octane_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Octane RAD status and analog output model";
    dc->vmsd = &vmstate_sgi_rad1_octane;
    pc->revision = 0xc0;
    pc->class_id = PCI_CLASS_NOT_DEFINED;
    pc->realize = sgi_rad1_octane_realize;
    pc->exit = sgi_rad1_audio_exit;
    rc->phases.hold = sgi_rad1_octane_reset_hold;
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
    {
        .name = TYPE_SGI_RAD1_OCTANE,
        .parent = TYPE_SGI_RAD1_AUDIO,
        .class_init = sgi_rad1_octane_class_init,
    },
};

DEFINE_TYPES(sgi_rad1_audio_types)
