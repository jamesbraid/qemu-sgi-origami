/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2026 James Braid
 * MACE 2.0 audio and AD1843 functional model.
 *
 * Implements the reviewed functional contract, including its selected digital
 * filters, virtual timing and finite staging. Electrical wiring, analog
 * quality, external synchronization and physical memory latency remain
 * unqualified.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/rcu.h"
#include "system/dma.h"
#include "migration/qemu-file.h"
#include "mace-audio.h"

#define AUDIO_RESET 0x400
#define AUDIO_ENABLE 0x200
#define AUDIO_POINTER 0xfe0
#define NEVER INT64_MAX
#define RATE_SCALE 7ULL
#define FIXED_RATE (48000 * RATE_SCALE)
/* Exact common units for Hz/7, nanoseconds and 1/3000-cycle phase work. */
#define CYCLE 21000000000LL
#define PHASE_UNIT (CYCLE / 3000)

enum {
    POWER_DOWN,
    POWER_STARTING,
    POWER_ON,
    POWER_STOPPING,
};

static const uint16_t codec_reset[32] = {
    0xc001, 0, 0, 0x8888, 0x8888, 0x8888, 0x8888, 0x8888,
    0x8868, 0x8888, 0x8888, 0, 0, 0x8080, 0x8080, 0,
    0x00ff, 0xbb80, 0, 0x00ff, 0xbb80, 0, 0x00ff, 0xbb80,
    0, 0, 0, 0x00c0, 0xc400, 0, 0, 0,
};

static const uint16_t codec_mask[32] = {
    0xc00f, 0x030f, 0xffff, 0x9f9f, 0x9f9f, 0x9f9f, 0x9f9f, 0x9f9f,
    0x9ffb, 0xbfbf, 0xbfbf, 0xbfbf, 0xbfbf, 0xbfbf, 0xbfbf, 0x0f0f,
    0xfcff, 0xffff, 0x01ff, 0xfcff, 0xffff, 0x01ff, 0xfcff, 0xffff,
    0x01ff, 0xc3f3, 0xcfff, 0x93d3, 0xffff, 0, 0, 0,
};

static void audio_schedule(MACEAudio *s);
static void codec_domains(MACEAudio *s);
static void codec_write(MACEAudio *s, unsigned reg, uint16_t value);
static void codec_clocks_update(MACEAudio *s, int64_t now);

static double audio_clip(double value)
{
    return MAX(-32768.0, MIN(32767.0, value));
}

static int16_t audio_quantize(double value)
{
    double rounded, fraction;
    int32_t integral;

    value = audio_clip(value);
    rounded = floor(value);
    fraction = value - rounded;
    integral = rounded;
    if (fraction > 0.5 || (fraction == 0.5 && (integral & 1))) {
        integral++;
    }
    return integral;
}

/* G.711 law conversion uses the standard piecewise companding intervals. */
static int16_t codec_decode(uint16_t slot, unsigned format)
{
    unsigned code = slot >> 8;
    int sample, segment;

    switch (format) {
    case 0:
        return ((int)code - 128) * 256;
    case 1:
        return (int16_t)slot;
    case 2:
        code = ~code & 0xff;
        sample = ((code & 15) * 8 + 132) << ((code >> 4) & 7);
        sample -= 132;
        return code & 128 ? -sample : sample;
    default:
        code ^= 0x55;
        segment = (code >> 4) & 7;
        sample = (code & 15) * 16 + (segment ? 264 : 8);
        if (segment > 1) {
            sample <<= segment - 1;
        }
        return code & 128 ? sample : -sample;
    }
}

static uint16_t codec_encode(int16_t sample, unsigned format)
{
    unsigned best = 0;
    int distance = INT_MAX;

    if (format == 0) {
        return ((sample + 32768) >> 8) << 8;
    }
    if (format == 1) {
        return (uint16_t)sample;
    }
    /* Exhaustive selection keeps clipping and segment boundaries symmetric. */
    for (unsigned code = 0; code < 256; code++) {
        int delta = abs(sample - codec_decode(code << 8, format));

        if (delta < distance ||
            (delta == distance && ((sample >= 0) == !!(code & 128)) &&
             ((sample >= 0) != !!(best & 128)))) {
            distance = delta;
            best = code;
        }
    }
    return best << 8;
}

static unsigned codec_selector(const uint16_t *reg, unsigned converter)
{
    return (reg[15] >> (converter < 2 ? converter * 2 :
                       8 + (converter - 2) * 2)) & 3;
}

static uint64_t codec_rate(const uint16_t *reg, unsigned converter)
{
    unsigned selector = codec_selector(reg, converter);
    unsigned mode;

    if (!selector) {
        return FIXED_RATE;
    }
    mode = reg[16 + 3 * (selector - 1)];
    if (!(reg[28] & (1 << (10 + selector))) || (mode & 0x8000)) {
        return 0;
    }
    return (uint64_t)reg[17 + 3 * (selector - 1)] *
           (mode & 0x800 ? 8 : RATE_SCALE);
}

static bool codec_enabled(const uint16_t *reg, unsigned converter)
{
    return reg[27] & (1 << (converter < 2 ? converter : converter + 6));
}

static bool codec_configuration_valid(const uint16_t *reg)
{
    unsigned enabled = reg[27];
    unsigned adc = enabled & 3;
    unsigned dac = (enabled >> 8) & 3;
    bool enhanced = reg[25] & 0x8000;
    uint64_t rate[4];
    uint64_t sdfs = (reg[26] & 0x80 ? 16384000ULL : 12288000ULL) *
                    RATE_SCALE / (reg[26] & 0x40 ? 256 : 512);

    if ((reg[26] & 0x10) && adc && adc != 3) {
        return false;
    }
    if (enhanced && (dac & 2)) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        unsigned filter = i < 2 ? i : i + 6;

        rate[i] = codec_rate(reg, i);
        if (!codec_enabled(reg, i)) {
            continue;
        }
        if (!rate[i] || rate[i] < 4000 * RATE_SCALE ||
            rate[i] > 54000 * RATE_SCALE ||
            (rate[i] > 49000 * RATE_SCALE &&
             (!(enabled & 0x8000) || (dac == 3 && adc)))) {
            return false;
        }
        if (!enhanced && (reg[25] & (1 << filter)) &&
            rate[i] > 24000 * RATE_SCALE) {
            return false;
        }
        if (i < 2 && ((reg[25] >> (4 + 2 * i)) & 3) == 1) {
            return false;
        }
        if (i >= 2) {
            uint16_t mix = reg[13 + i - 2];

            for (unsigned side = 0; side < 2; side++) {
                unsigned byte = (mix >> (side ? 0 : 8)) & 255;

                if (!(byte & 128) && (byte & 63) != 63 &&
                    (codec_selector(reg, side) != codec_selector(reg, i) ||
                     codec_rate(reg, side) != rate[i] || sdfs < rate[i])) {
                    return false;
                }
            }
        }
    }
    return !(reg[26] & 0x10) || !adc || rate[0] == rate[1];
}

static uint16_t audio_depth(MACEAudioChannel *c)
{
    return (c->write - c->read) & AUDIO_POINTER;
}

static bool audio_threshold(MACEAudioChannel *c, unsigned channel)
{
    unsigned blocks = audio_depth(c) >> 5;
    unsigned mode = (c->control >> 5) & 7;

    if (c->control & AUDIO_RESET) {
        return false;
    }
    switch (mode) {
    case 1:
    case 2:
    case 3:
        return channel ? blocks < mode * 32 : blocks >= mode * 32;
    case 4:
        return blocks == 0;
    case 5:
        return blocks != 0;
    case 6:
        return blocks == 127;
    case 7:
        return blocks != 127;
    default:
        return false;
    }
}

static void audio_update_irq(MACEAudio *s)
{
    uint32_t status = (!!(s->response & s->mask)) |
                      (!!(s->control & 0x1800000) << 1);

    for (unsigned i = 0; i < 3; i++) {
        MACEAudioChannel *c = &s->channel[i];

        status |= audio_threshold(c, i) << (2 + i * 2);
        if (!(c->control & AUDIO_RESET)) {
            status |= c->fault && (i || c->fault == 1) ?
                      1U << (3 + i * 2) : 0;
        }
    }
    s->update_irq(s->opaque, status);
}

static void audio_stamp(MACEAudio *s, unsigned channel)
{
    MACEAudioChannel *c = &s->channel[channel];

    if (!(c->control & AUDIO_RESET)) {
        c->msc++;
        c->ust = s->get_ust(s->opaque, s->next_serial);
    }
}

static bool audio_dma(MACEAudio *s, unsigned channel, bool write)
{
    MACEAudioChannel *c = &s->channel[channel];
    hwaddr addr = (*s->ring_base & 0xffff8000U) | (channel << 12) |
                  (write ? c->write : c->read);
    hwaddr translated, length = 32;
    MemoryRegion *mr;
    MemTxResult result;

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(s->as, addr, &translated, &length, write,
                                  MEMTXATTRS_UNSPECIFIED);
    if (length < 32 || !memory_region_is_ram(mr) ||
        memory_region_is_rom(mr) ||
        !address_space_access_valid(s->as, addr, 32, write,
                                     MEMTXATTRS_UNSPECIFIED)) {
        hwaddr failed = length < 32 && memory_region_is_ram(mr) ?
                         addr + length : addr;
        hwaddr failed_length = 1;
        uint8_t ignored = 0;
        MemoryRegion *failed_mr = address_space_translate(
            s->as, failed, &translated, &failed_length, write,
            MEMTXATTRS_UNSPECIFIED);

        /*
         * This bus contains only RAM and its decoder's error responders.
         * Reach the failing responder without publishing a partial RAM block.
         */
        if (!memory_region_is_ram(failed_mr)) {
            if (write) {
                dma_memory_write(s->as, failed, &ignored, 1,
                                  MEMTXATTRS_UNSPECIFIED);
            } else {
                dma_memory_read(s->as, failed, &ignored, 1,
                                 MEMTXATTRS_UNSPECIFIED);
            }
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-audio: inaccessible channel %u DMA at 0x%"
                      HWADDR_PRIx "\n", channel, addr);
        c->fault = 2;
        return false;
    }
    result = write ? dma_memory_write(s->as, addr, c->data, 32,
                                      MEMTXATTRS_UNSPECIFIED) :
                     dma_memory_read(s->as, addr, c->data, 32,
                                     MEMTXATTRS_UNSPECIFIED);
    if (result != MEMTX_OK) {
        c->fault = 2;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-audio: channel %u memory transaction failed\n",
                      channel);
        return false;
    }
    if (write) {
        c->write = (c->write + 32) & AUDIO_POINTER;
        c->count = 0;
    } else {
        c->read = (c->read + 32) & AUDIO_POINTER;
        c->count = 4;
        c->cursor = 0;
    }
    return true;
}

static void audio_capture_drain(MACEAudio *s)
{
    MACEAudioChannel *c = &s->channel[0];

    if ((c->control & (AUDIO_RESET | AUDIO_ENABLE)) == AUDIO_ENABLE &&
        !c->fault && c->count == 4 && audio_depth(c) != AUDIO_POINTER) {
        audio_dma(s, 0, true);
    }
}

static uint16_t audio_playback_slot(uint32_t raw)
{
    unsigned extension = raw >> 24;

    if (extension != ((raw & 0x80000000U) ? 255 : 0)) {
        return 0xffff;
    }
    return (raw >> 8) & 0xffff;
}

static void audio_playback(MACEAudio *s, unsigned channel, uint16_t *slot)
{
    MACEAudioChannel *c = &s->channel[channel];

    slot[0] = slot[1] = 0;
    if ((c->control & (AUDIO_RESET | AUDIO_ENABLE)) != AUDIO_ENABLE ||
        c->fault) {
        return;
    }
    if (!c->count && audio_depth(c) && !audio_dma(s, channel, false)) {
        return;
    }
    if (c->count) {
        slot[0] = audio_playback_slot(ldl_be_p(c->data + c->cursor * 8));
        slot[1] = audio_playback_slot(ldl_be_p(c->data + c->cursor * 8 + 4));
        c->cursor++;
        c->count--;
    }
}

static void audio_capture(MACEAudio *s, uint16_t left, uint16_t right)
{
    MACEAudioChannel *c = &s->channel[0];
    uint32_t lanes[2] = {
        ((uint32_t)(int32_t)(int16_t)left << 8) | 255,
        ((uint32_t)(int32_t)(int16_t)right << 8) | 255,
    };

    audio_stamp(s, 0);
    if ((c->control & (AUDIO_RESET | AUDIO_ENABLE)) != AUDIO_ENABLE ||
        c->fault) {
        return;
    }
    audio_capture_drain(s);
    if (c->fault) {
        return;
    }
    if (c->count == 4) {
        c->fault = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-audio: capture staging overflow\n");
        return;
    }
    stl_be_p(c->data + c->count * 8, lanes[0]);
    stl_be_p(c->data + c->count * 8 + 4, lanes[1]);
    c->count++;
    audio_capture_drain(s);
}

static double codec_gain(uint16_t reg, unsigned side, unsigned bits,
                          double initial, bool mix)
{
    unsigned value = (reg >> (side ? 0 : 8)) & 255;
    unsigned code = value & ((1 << bits) - 1);

    if ((value & 128) || (mix && code == 63)) {
        return 0;
    }
    return pow(10.0, (initial - 1.5 * code) / 20.0);
}

static double converter_interpolate(MACEAudioConverter *c, unsigned side,
                                     int64_t now)
{
    double fraction;

    if (!c->rate || !c->sample_time) {
        return 0;
    }
    fraction = (double)(now - c->sample_time) * c->rate /
               (1000000000.0 * RATE_SCALE);
    fraction = MIN(1.0, MAX(0.0, fraction));
    return c->previous[side] +
           (c->sample[side] - c->previous[side]) * fraction;
}

static double converter_output(MACEAudioConverter *c, unsigned side,
                                int64_t now)
{
    double fraction;

    if (!c->rate || !c->output_time) {
        return 0;
    }
    fraction = (double)(now - c->output_time) * c->rate /
               (1000000000.0 * RATE_SCALE);
    fraction = MIN(1.0, MAX(0.0, fraction));
    return c->output_previous[side] +
           (c->output[side] - c->output_previous[side]) * fraction;
}

static void codec_analog(MACEAudio *s, int64_t now)
{
    bool analog = s->power_state == POWER_ON && (s->reg[27] & 128);
    bool mixer = analog && (s->reg[27] & 16);
    bool all_mute = s->reg[8] & 128;

    for (unsigned side = 0; side < 2; side++) {
        double dac[2];
        double sum = 0;

        for (unsigned d = 0; d < 2; d++) {
            MACEAudioConverter *c = &s->converter[2 + d];

            dac[d] = analog && c->active ?
                     converter_output(c, side, now) : 0;
        }
        if (mixer) {
            if (!all_mute) {
                for (unsigned node = 0; node < 4; node++) {
                    sum += s->input[MACE_AUDIO_AUX1 + node][side] *
                           codec_gain(s->reg[4 + node], side, 5, 12, false);
                }
                if ((s->reg[27] & 0x1000) && codec_enabled(s->reg, 3)) {
                    sum += dac[1] *
                           codec_gain(s->reg[3], side, 5, 12, false);
                }
            }
            sum += s->input[MACE_AUDIO_MONO][side] *
                   codec_gain(s->reg[8], 0, 5, 12, false);
            if (!(s->reg[8] & 8)) {
                sum += s->input[MACE_AUDIO_SUM][side];
            }
        }
        s->line[0][side] = audio_clip(sum + dac[0] *
            codec_gain(s->converter[2].applied_gain, side, 6, 12, false));
        s->line[1][side] = audio_clip(dac[1] *
            codec_gain(s->converter[3].applied_gain, side, 6, 12, false));
        s->headphone[side] = analog && (s->reg[27] & 64) &&
                             !(s->reg[8] & 32) ?
                             s->line[0][side] *
                             (s->reg[8] & 16 ? 1.0 : 0.5) : 0;
    }
    s->mono = !(s->reg[8] & 64) ?
              (s->line[0][0] + s->line[0][1]) * 0.5 : 0;
    if (!mixer) {
        s->mono = s->pdmnft ? (s->input[MACE_AUDIO_MONO][0] +
                              s->input[MACE_AUDIO_MONO][1]) * 0.5 : 0;
    }
}

static void codec_peak(MACEAudio *s, unsigned side, double value)
{
    double amplitude = fabs(value);
    unsigned peak = amplitude > 36765 ? 3 : amplitude > 32767 ? 2 :
                    amplitude >= 29204 ? 1 : 0;
    unsigned shift = side * 2;

    s->reg[1] = (s->reg[1] & ~(3 << shift)) |
                (MAX((s->reg[1] >> shift) & 3, peak) << shift);
}

static void codec_convert(MACEAudio *s, unsigned index, int64_t now)
{
    MACEAudioConverter *c = &s->converter[index];
    double sample[2] = { 0, 0 };

    codec_analog(s, now);
    if (index < 2) {
        unsigned side = index;
        unsigned source = (s->reg[25] >> (4 + side * 2)) & 3;
        unsigned analog = (s->reg[2] >> (side ? 5 : 13)) & 7;
        unsigned byte = s->reg[2] >> (side ? 0 : 8);
        double value;

        if (source >= 2) {
            value = codec_enabled(s->reg, source) &&
                    s->converter[source].active ?
                    converter_interpolate(&s->converter[source], side, now) : 0;
        } else {
            switch (analog) {
            case 0:
                value = s->input[MACE_AUDIO_LINE][side] -
                        (s->reg[28] & (1 << side) ?
                         s->line_negative[side] : 0);
                break;
            case 1:
                value = s->input[MACE_AUDIO_MIC][side];
                break;
            case 2:
            case 3:
            case 4:
                value = s->input[analog - 1][side];
                break;
            case 5:
                value = s->input[MACE_AUDIO_MONO][side];
                break;
            default:
                value = s->line[analog - 6][side];
                break;
            }
            value *= pow(10.0, (1.5 * (byte & 15) +
                         (analog == 1 && (byte & 16) ? 20 : 0)) / 20.0);
        }
        codec_peak(s, side, value);
        sample[side] = audio_clip(value);
        if (c->count < 4) {
            unsigned tail = (c->head + c->count) & 3;

            c->fifo[tail] = codec_encode(audio_quantize(value),
                                        (s->reg[26] >> (side * 2)) & 3);
            c->count++;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-mace-audio: ADC serial FIFO overflow\n");
        }
    } else {
        unsigned dac = index - 2;

        if (c->count) {
            sample[0] = c->fifo[c->head * 2];
            sample[1] = c->fifo[c->head * 2 + 1];
            c->head = (c->head + 1) & 3;
            c->count--;
        } else {
            s->reg[1] |= 1 << (8 + dac);
        }
        for (unsigned side = 0; side < 2; side++) {
            MACEAudioConverter *adc = &s->converter[side];
            double input = adc->sample_time == now ? adc->previous[side] :
                                                   adc->sample[side];

            sample[side] += input *
                           codec_gain(s->reg[13 + dac], side, 6, 0, true);
            sample[side] = audio_clip(sample[side]) *
                           codec_gain(s->reg[11 + dac], side, 6, 0, false);
        }
    }
    memcpy(c->previous, c->sample, sizeof(c->sample));
    memcpy(c->sample, sample, sizeof(sample));
    c->sample_time = now;
    c->converted++;
    if (index >= 2) {
        double output[2];

        for (unsigned side = 0; side < 2; side++) {
            output[side] = sample[side];
            if (s->reg[25] & 0x4000) {
                output[side] = 0;
                for (unsigned d = 2; d < 4; d++) {
                    if (codec_enabled(s->reg, d) && s->converter[d].active) {
                        output[side] += converter_interpolate(&s->converter[d],
                                                              side, now);
                    }
                }
                output[side] = audio_clip(output[side]);
            }
        }
        if (c->gain_wait) {
            bool crossing = false;

            for (unsigned side = 0; side < 2; side++) {
                crossing |= (output[side] <= 0 && c->output[side] >= 0) ||
                            (output[side] >= 0 && c->output[side] <= 0);
            }
            if (crossing || !--c->gain_wait) {
                c->applied_gain = c->pending_gain;
                c->gain_wait = 0;
            }
        }
        memcpy(c->output_previous, c->output, sizeof(c->output));
        memcpy(c->output, output, sizeof(output));
        c->output_time = now;
    }
    codec_analog(s, now);
}

static void audio_host_sample(MACEAudio *s, int64_t now)
{
    uint64_t period = 1000000000ULL + s->host_fraction;

    codec_analog(s, now);
    if (s->voice_out) {
        if (s->host_count < ARRAY_SIZE(s->host_output) / 2) {
            unsigned tail = (s->host_head + s->host_count) %
                            (ARRAY_SIZE(s->host_output) / 2);

            s->host_output[tail * 2] = audio_quantize(s->line[0][0]);
            s->host_output[tail * 2 + 1] = audio_quantize(s->line[0][1]);
            s->host_count++;
        }
    }
    s->next_host = now + period / 48000;
    s->host_fraction = period % 48000;
}

static void codec_converters_update(MACEAudio *s)
{
    for (unsigned i = 0; i < 4; i++) {
        MACEAudioConverter *c = &s->converter[i];
        bool enabled = s->power_state == POWER_ON &&
                       codec_enabled(s->reg, i);
        uint64_t rate = codec_rate(s->reg, i);

        if (!enabled) {
            c->active = false;
            c->edges = 0;
            c->count = 0;
            c->head = 0;
            c->requested = false;
            c->sample[0] = c->sample[1] = 0;
            c->previous[0] = c->previous[1] = 0;
            c->output[0] = c->output[1] = 0;
            c->output_previous[0] = c->output_previous[1] = 0;
            if (i < 2) {
                s->reg[1] &= ~(3 << (i * 2));
            } else {
                s->reg[1] &= ~(1 << (i + 6));
            }
        } else if (!c->active) {
            if (!c->edges) {
                c->edges = 6;
            }
        }
        c->rate = rate;
    }
}

static int64_t codec_work_deadline(MACEAudio *s, uint64_t index)
{
    uint64_t ns = muldiv64(index, 1000000000, 3072000);
    uint64_t residue = ((index % 3072000) * 1000000000ULL) % 3072000;

    return s->reference_ready + ns + !!residue;
}

static void codec_work_start(MACEAudio *s, MACEAudioClock *c, int64_t now)
{
    if (!c->rate || !(c->phase & 255)) {
        c->phase_next = NEVER;
        return;
    }
    c->work_index = muldiv64(now - s->reference_ready, 3072000,
                             1000000000) + 1;
    c->phase_next = codec_work_deadline(s, c->work_index);
}

static void codec_clock_advance(MACEAudioClock *c, int64_t now)
{
    c->progress += (now - c->time) * (int64_t)c->rate * 3;
    c->time = now;
}

static void codec_clock_schedule(MACEAudioClock *c, int64_t now)
{
    if (!c->rate) {
        c->next = NEVER;
        return;
    }
    c->next = now + DIV_ROUND_UP(CYCLE - c->progress, c->rate * 3);
}

static void codec_clock_edge(MACEAudio *s, unsigned clock, int64_t now)
{
    for (unsigned i = 0; i < 4; i++) {
        unsigned selector = codec_selector(s->reg, i);
        unsigned selected = selector ? selector - 1 : 3;

        if (s->converter[i].active && selected == clock) {
            codec_convert(s, i, now);
        }
    }
}

static void codec_clocks_update(MACEAudio *s, int64_t now)
{
    for (unsigned g = 0; g < 3; g++) {
        MACEAudioClock *c = &s->clock[g];
        bool enabled = s->reg[28] & (1 << (11 + g));
        bool internal = !(s->reg[16 + g * 3] & 0x8000);
        uint64_t rate = enabled && internal ?
                        (uint64_t)s->reg[17 + g * 3] *
                        (s->reg[16 + g * 3] & 0x800 ? 8 : RATE_SCALE) : 0;

        if (c->rate == rate) {
            continue;
        }
        if (c->rate) {
            codec_clock_advance(c, now);
        } else {
            c->time = now;
            c->progress = 0;
        }
        c->rate = rate;
        codec_clock_schedule(c, now);
        if (!enabled) {
            c->phase = c->phase_latch = 0;
        }
        if (!c->rate) {
            c->phase_next = NEVER;
        } else if (c->phase_next == NEVER) {
            codec_work_start(s, c, now);
        }
    }
}

bool mace_audio_frequency(MACEAudio *s, unsigned generator, bool bit,
                          uint64_t *numerator, uint64_t *denominator)
{
    static const unsigned bit_basis[] = {
        2400, 4800, 7200, 9600, 12000, 14400,
        16800, 19200, 21600, 24000, 26400, 28800,
    };
    /* Conversion bases use sevenths of Hz for the 8/7 table entries. */
    static const unsigned conversion_basis[] = {
        50400, 58800, 63000, 67200, 78400, 84000,
        89600, 57600, 72000, 76800, 96000,
    };
    uint16_t mode = s->reg[16 + 3 * generator];
    uint64_t frequency = (uint64_t)s->reg[17 + 3 * generator] *
                         (mode & 0x800 ? 8 : RATE_SCALE);
    unsigned low = mode & 15;
    unsigned high = (mode >> 4) & 15;

    if (mode & 0x8000) {
        return false;
    }
    if (!bit) {
        *numerator = frequency * (mode & 0x400 ? 128 : 1);
        *denominator = RATE_SCALE;
    } else if (low == 15 && high == 15) {
        *numerator = 128 * frequency + (mode & 0x1000 ? 1400 : 0);
        *denominator = RATE_SCALE;
    } else if (low < ARRAY_SIZE(bit_basis) &&
               high < ARRAY_SIZE(conversion_basis)) {
        *numerator = bit_basis[low] * frequency;
        *denominator = conversion_basis[high];
        if (mode & 0x1000) {
            *numerator += 200 * *denominator;
        }
    } else {
        return false;
    }
    return true;
}

static void codec_domains(MACEAudio *s)
{
    bool down = s->reg[0] & 0x4000;
    bool analog = !down && (s->reg[27] & 128);
    bool mixer = analog && (s->reg[27] & 16);

    if (down) {
        for (unsigned i = 1; i <= 15; i++) {
            s->reg[i] = codec_reset[i];
        }
        s->reg[25] = codec_reset[25];
        s->reg[27] = codec_reset[27];
    }
    if (!analog || (s->reg[27] & 0x1200) != 0x1200) {
        s->reg[3] = codec_reset[3];
    }
    if (!mixer) {
        for (unsigned i = 4; i <= 7; i++) {
            s->reg[i] = codec_reset[i];
        }
        s->reg[8] = (s->reg[8] & 255) | (codec_reset[8] & 0xff00);
    }
    for (unsigned d = 0; d < 2; d++) {
        MACEAudioConverter *c = &s->converter[2 + d];

        if (!analog || !codec_enabled(s->reg, d + 2)) {
            s->reg[9 + d] = codec_reset[9 + d];
            c->applied_gain = c->pending_gain = codec_reset[9 + d];
            c->gain_wait = 0;
        }
    }
    for (unsigned g = 0; g < 3; g++) {
        if (!(s->reg[28] & (1 << (11 + g)))) {
            s->clock[g].phase = 0;
            s->clock[g].phase_latch = 0;
            s->reg[18 + 3 * g] = 0;
        }
    }
}

static uint16_t codec_read(MACEAudio *s, unsigned reg, bool phase_latch)
{
    uint16_t value;

    if (reg == 18 || reg == 21 || reg == 24) {
        MACEAudioClock *c = &s->clock[(reg - 18) / 3];

        return phase_latch ? c->phase_latch : c->phase;
    }
    value = s->reg[reg];
    if (reg == 1) {
        s->reg[1] = 0;
    }
    return value;
}

static void codec_power_start(MACEAudio *s, int64_t now)
{
    s->power_state = POWER_STARTING;
    s->power_deadline = now + 470000000 +
                        (!s->calibrated || (s->reg[28] & 0x4000) ?
                         4000000 : 0);
    s->reg[0] |= 0x4000;
}

static void codec_write(MACEAudio *s, unsigned reg, uint16_t value)
{
    uint16_t candidate[32];
    uint16_t old = s->reg[reg];
    int64_t now = s->next_serial;
    bool down = s->reg[0] & 0x4000;

    if (value & ~codec_mask[reg]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-audio: register %u reserved data 0x%04x\n",
                      reg, value & ~codec_mask[reg]);
    }
    value &= codec_mask[reg];
    if (!reg || reg >= 29 || (s->control & 1) || s->pwrdwn ||
        s->pwrdwn_pending ||
        (s->reg[0] & 0x8000)) {
        return;
    }
    if (down && !((reg >= 16 && reg <= 24) || reg == 26 || reg == 28)) {
        return;
    }
    if (reg == 1) {
        s->reg[1] = 0;
        return;
    }
    if (reg == 28) {
        if (s->power_state == POWER_STARTING ||
            s->power_state == POWER_STOPPING) {
            value = (value & ~0x8000) | (old & 0x8000);
        }
        if (!s->calibrated) {
            value |= 0x4000;
        }
    }
    memcpy(candidate, s->reg, sizeof(candidate));
    candidate[reg] = value;
    if ((reg == 25 &&
         (((old ^ value) & 0x8000 && (s->reg[27] & 0x303)) ||
          ((old ^ value) & 0x4000 && (s->reg[27] & 0x300)) ||
          ((old ^ value) & s->reg[27] & 0x303))) ||
        (reg == 26 && !(old & 16) && (value & 16) &&
         (s->reg[27] & 3)) ||
        !codec_configuration_valid(candidate)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace-audio: unsupported active codec configuration "
                      "at register %u\n", reg);
        return;
    }
    s->reg[reg] = value;
    if (reg == 18 || reg == 21 || reg == 24) {
        MACEAudioClock *c = &s->clock[(reg - 18) / 3];

        c->phase = value;
        codec_work_start(s, c, now);
    }
    if (reg == 9 || reg == 10) {
        unsigned d = reg - 9;
        MACEAudioConverter *c = &s->converter[2 + d];

        c->pending_gain = value;
        if (s->reg[8] & (1 << d)) {
            c->applied_gain = value;
            c->gain_wait = 0;
        } else if (old != value) {
            c->gain_wait = 384;
        }
    }
    if (reg == 28 && ((old ^ value) & 0x8000)) {
        if (value & 0x8000) {
            s->power_state = POWER_STOPPING;
            s->power_deadline = now + 5000000;
            s->reg[0] |= 0x4000;
        } else {
            codec_power_start(s, now);
        }
    }
    codec_domains(s);
    codec_clocks_update(s, now);
    codec_converters_update(s);
    if (s->update_ttl) {
        s->update_ttl(s->opaque, (s->reg[28] >> 8) & 3);
    }
}

static void codec_reset_state(MACEAudio *s, int64_t now)
{
    memcpy(s->reg, codec_reset, sizeof(s->reg));
    memset(s->clock, 0, sizeof(s->clock));
    memset(s->converter, 0, sizeof(s->converter));
    memset(s->line, 0, sizeof(s->line));
    memset(s->headphone, 0, sizeof(s->headphone));
    s->mono = 0;
    s->power_state = POWER_DOWN;
    s->power_deadline = NEVER;
    s->init_deadline = NEVER;
    s->calibrated = false;
    s->presence = false;
    s->response_pending = false;
    s->transaction_valid = false;
    s->pending_response = 0;
    s->current_response = 0;
    s->latched_command = 0;
    s->next_serial = NEVER;
    s->next_host = NEVER;
    s->host_fraction = 0;
    s->serial_fraction = 0;
    s->serial_stage = 0;
    s->group = 0;
    s->frames = 0;
    s->sclk = 12288000;
    s->frame_slots = 32;
    s->reset_start = now;
    memset(s->playback_valid, 0, sizeof(s->playback_valid));
    memset(s->capture_valid, 0, sizeof(s->capture_valid));
    for (unsigned i = 0; i < 4; i++) {
        s->converter[i].rate = FIXED_RATE;
        s->clock[i].next = s->clock[i].phase_next = NEVER;
        s->clock[i].time = now;
        if (i >= 2) {
            s->converter[i].applied_gain = codec_reset[9 + i - 2];
            s->converter[i].pending_gain = codec_reset[9 + i - 2];
        }
    }
    s->host_head = s->host_count = 0;
    if (s->update_ttl) {
        s->update_ttl(s->opaque, 0);
    }
}

static void codec_serial_advance(MACEAudio *s, unsigned clocks)
{
    uint64_t time = (uint64_t)clocks * 1000000000 + s->serial_fraction;

    s->next_serial += time / s->sclk;
    s->serial_fraction = time % s->sclk;
}

static void codec_deliver_playback(MACEAudio *s, unsigned dac)
{
    MACEAudioConverter *c = &s->converter[2 + dac];

    if (s->playback_valid[dac] && codec_enabled(s->reg, 2 + dac)) {
        uint16_t slot[2];
        unsigned tail = (c->head + c->count) & 3;
        unsigned format = (s->reg[26] >> (8 + dac * 2)) & 3;

        audio_playback(s, 1 + dac, slot);
        if (c->count < 4) {
            c->fifo[tail * 2] = codec_decode(slot[0], format);
            c->fifo[tail * 2 + 1] = codec_decode(
                s->reg[26] & (1 << (14 + dac)) ? slot[0] : slot[1], format);
            c->count++;
        }
    }
}

static void codec_serial_event(MACEAudio *s)
{
    unsigned reg;

    switch (s->serial_stage) {
    case 0:
        if (!s->group) {
            s->frames++;
            for (unsigned i = 0; i < 4; i++) {
                MACEAudioConverter *c = &s->converter[i];

                if (c->edges && !--c->edges) {
                    c->active = true;
                }
            }
        }
        s->presence = true;
        codec_serial_advance(s, 1);
        break;
    case 1:
        if (!s->group) {
            for (unsigned g = 0; g < 3; g++) {
                s->clock[g].phase_latch = s->clock[g].phase;
            }
        }
        s->latched_command = s->command;
        reg = (s->latched_command >> 17) & 127;
        s->transaction_valid = reg < 32;
        if (!s->transaction_valid) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace-audio: unsupported codec address %u\n",
                          reg);
        } else if (s->latched_command & 0x10000) {
            s->current_response = codec_read(s, reg, true);
        }
        /* Do not overwrite the response still due at this group's slot 1. */
        for (unsigned d = 0; d < 2; d++) {
            MACEAudioConverter *c = &s->converter[2 + d];

            s->playback_valid[d] = c->requested && c->count < 4;
            c->requested = codec_enabled(s->reg, 2 + d) &&
                           c->count + s->playback_valid[d] < 4;
        }
        for (unsigned side = 0; side < 2; side++) {
            MACEAudioConverter *c = &s->converter[side];

            s->capture_valid[side] = c->count != 0;
            if (c->count) {
                s->capture_slot[side] = c->fifo[c->head];
                c->head = (c->head + 1) & 3;
                c->count--;
            }
        }
        codec_serial_advance(s, 16);
        break;
    case 2:
        for (unsigned d = 0; d < 2; d++) {
            if (s->converter[2 + d].requested) {
                audio_stamp(s, 1 + d);
            }
        }
        if (s->response_pending) {
            s->response = s->pending_response;
            s->response_pending = false;
        }
        if (s->transaction_valid && (s->latched_command & 0x10000)) {
            s->pending_response = s->current_response;
            s->response_pending = true;
        }
        codec_serial_advance(s, 16);
        break;
    case 3:
        codec_deliver_playback(s, 0);
        if (s->capture_valid[0] && s->capture_valid[1]) {
            audio_capture(s, s->capture_slot[0], s->capture_slot[1]);
        } else if (s->capture_valid[0] || s->capture_valid[1]) {
            MACEAudioChannel *c = &s->channel[0];

            if ((c->control & (AUDIO_RESET | AUDIO_ENABLE)) == AUDIO_ENABLE &&
                !c->fault) {
                c->fault = 3;
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace-audio: partial-valid stereo capture\n");
            }
        }
        codec_serial_advance(s, 32);
        break;
    case 4:
        codec_deliver_playback(s, 1);
        codec_serial_advance(s, 32);
        break;
    case 5:
        if (s->transaction_valid && !(s->latched_command & 0x10000)) {
            reg = (s->latched_command >> 17) & 31;
            s->pending_response = codec_read(s, reg, false);
            s->response_pending = true;
            codec_write(s, reg, s->latched_command & 0xffff);
        }
        codec_analog(s, s->next_serial);
        if ((s->reg[26] & 128 ? 16384000U : 12288000U) != s->sclk) {
            unsigned rate = s->reg[26] & 128 ? 16384000 : 12288000;

            s->serial_fraction = muldiv64(s->serial_fraction, rate, s->sclk);
            s->sclk = rate;
        }
        codec_serial_advance(s, 159);
        break;
    }
    if (++s->serial_stage == 6) {
        s->serial_stage = 0;
        s->group = s->frame_slots == 32 ? !s->group : 0;
        if (!s->group || (s->reg[26] & 32)) {
            s->frame_slots = s->reg[26] & 64 ? 16 : 32;
            if (s->frame_slots == 16) {
                s->group = 0;
            }
        }
    }
}

static int64_t audio_next(MACEAudio *s)
{
    int64_t next = MIN(s->next_serial,
                       MIN(s->init_deadline, s->power_deadline));

    next = MIN(next, s->next_host);

    for (unsigned g = 0; g < 4; g++) {
        next = MIN(next, MIN(s->clock[g].next, s->clock[g].phase_next));
    }
    return next;
}

void mace_audio_sync(MACEAudio *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t event;

    while ((event = audio_next(s)) != NEVER && event <= now) {
        if (s->init_deadline == event) {
            s->init_deadline = NEVER;
            s->reg[0] &= ~0x8000;
            s->next_serial = event;
            s->reference_ready = event;
            s->clock[3].time = event;
            s->clock[3].progress = 0;
            s->clock[3].rate = FIXED_RATE;
            codec_clock_schedule(&s->clock[3], event);
            if (s->voice_out) {
                s->next_host = event;
            }
        }
        if (s->power_deadline == event) {
            s->power_deadline = NEVER;
            if (s->power_state == POWER_STARTING) {
                s->calibrated = true;
                if (s->pwrdwn_pending) {
                    s->power_state = POWER_STOPPING;
                    s->power_deadline = event + 5000000;
                } else {
                    s->power_state = POWER_ON;
                    s->reg[0] &= ~0x4000;
                }
            } else {
                s->power_state = POWER_DOWN;
                if (s->pwrdwn_pending) {
                    codec_reset_state(s, event);
                    s->pwrdwn_pending = false;
                    if (!s->pwrdwn && !(s->control & 1)) {
                        s->init_deadline = event + 600000;
                    }
                }
            }
            codec_domains(s);
            codec_converters_update(s);
        }
        for (unsigned g = 0; g < 4; g++) {
            MACEAudioClock *c = &s->clock[g];

            if (c->next == event) {
                codec_clock_advance(c, event);
                c->progress -= CYCLE;
                codec_clock_edge(s, g, event);
                codec_clock_schedule(c, event);
            }
        }
        for (unsigned g = 0; g < 3; g++) {
            MACEAudioClock *c = &s->clock[g];

            if (c->phase_next == event) {
                codec_clock_advance(c, event);
                c->progress += c->phase & 256 ? -PHASE_UNIT : PHASE_UNIT;
                c->phase = (c->phase & 256) | ((c->phase & 255) - 1);
                if (c->progress >= CYCLE) {
                    c->progress -= CYCLE;
                    codec_clock_edge(s, g, event);
                }
                codec_clock_schedule(c, event);
                c->work_index++;
                c->phase_next = c->phase & 255 ?
                                codec_work_deadline(s, c->work_index) : NEVER;
            }
        }
        if (s->next_serial == event) {
            codec_serial_event(s);
        }
        if (s->next_host == event) {
            audio_host_sample(s, event);
        }
        audio_update_irq(s);
    }
    audio_schedule(s);
}

static void audio_timer(void *opaque)
{
    mace_audio_sync(opaque);
}

static void audio_schedule(MACEAudio *s)
{
    int64_t next = audio_next(s);

    if (next == NEVER) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, next);
    }
}

static bool audio_access_valid(hwaddr offset, unsigned size)
{
    if ((size != 4 && size != 8) || (offset & (size - 1))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-audio: invalid PIO offset 0x%" HWADDR_PRIx
                      " width %u\n", offset, size);
        return false;
    }
    return true;
}

uint64_t mace_audio_read(MACEAudio *s, hwaddr offset, unsigned size)
{
    uint32_t value = 0;
    unsigned reg = (offset & 0x78) / 8;

    mace_audio_sync(s);
    if (!audio_access_valid(offset, size)) {
        return 0;
    }
    switch (reg) {
    case 0:
        value = (s->control & 0x1800001) | (s->presence << 1) |
                ((s->channel[0].write >> 5) << 2) |
                ((s->channel[1].read >> 5) << 9) |
                ((s->channel[2].read >> 5) << 16);
        break;
    case 1:
        value = s->command;
        break;
    case 2:
        value = s->mask;
        break;
    case 3:
        value = s->response;
        break;
    default: {
        MACEAudioChannel *c = &s->channel[reg / 4 - 1];

        switch (reg & 3) {
        case 0:
            value = c->control;
            break;
        case 1:
            value = c->read;
            break;
        case 2:
            value = c->write;
            break;
        case 3:
            value = audio_depth(c);
            break;
        }
        break;
    }
    }
    return size == 4 && !(offset & 4) ? 0 : value;
}

void mace_audio_write(MACEAudio *s, hwaddr offset, uint64_t value,
                      unsigned size)
{
    unsigned reg = (offset & 0x78) / 8;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    mace_audio_sync(s);
    if (!audio_access_valid(offset, size) ||
        (size == 4 && !(offset & 4))) {
        return;
    }
    switch (reg) {
    case 0: {
        bool asserting = value & 1;
        bool old_reset = s->control & 1;

        s->control &= (value & 0x1800000) | ~0x1800000U;
        s->control = (s->control & ~1U) | asserting;
        if (asserting && !old_reset) {
            codec_reset_state(s, now);
            s->pwrdwn_pending = false;
        } else if (!asserting && old_reset && !s->pwrdwn) {
            if (now - s->reset_start >= 100) {
                s->init_deadline = now + 600000;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-mace-audio: codec reset shorter than 100ns\n");
            }
        }
        break;
    }
    case 1:
        s->command = value & 0xffffff;
        break;
    case 2:
        s->mask = value & 0xffff;
        break;
    case 3:
        break;
    default: {
        unsigned channel = reg / 4 - 1;
        MACEAudioChannel *c = &s->channel[channel];

        switch (reg & 3) {
        case 0:
            if (value & AUDIO_RESET) {
                memset(c, 0, sizeof(*c));
                c->control = AUDIO_RESET;
            } else {
                c->control = value & 0x6e0;
            }
            break;
        case 1:
        case 2:
            if (!(c->control & AUDIO_RESET) &&
                (!((reg & 3) == (channel ? 1 : 2)) ||
                 !(c->control & AUDIO_ENABLE))) {
                if ((reg & 3) == 1) {
                    c->read = value & AUDIO_POINTER;
                } else {
                    c->write = value & AUDIO_POINTER;
                }
            }
            break;
        case 3:
            break;
        }
        if (!channel && (reg & 3) != 3) {
            audio_capture_drain(s);
        }
        break;
    }
    }
    audio_update_irq(s);
    audio_schedule(s);
}

static uint64_t audio_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    return mace_audio_read(opaque, offset, size);
}

static void audio_mmio_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    mace_audio_write(opaque, offset, value, size);
}

const MemoryRegionOps mace_audio_ops = {
    .read = audio_mmio_read,
    .write = audio_mmio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

uint64_t mace_audio_timestamp_read(MACEAudio *s, unsigned channel,
                                   unsigned half, unsigned size)
{
    MACEAudioChannel *c = &s->channel[channel];

    mace_audio_sync(s);
    if (size == 8) {
        return ((uint64_t)c->ust << 32) | c->msc;
    }
    return half ? c->msc : c->ust;
}

void mace_audio_timestamp_write(MACEAudio *s, unsigned channel,
                                unsigned half, unsigned size, uint64_t value)
{
    MACEAudioChannel *c = &s->channel[channel];

    mace_audio_sync(s);
    if (size == 8 || !half) {
        c->ust = size == 8 ? value >> 32 : value;
    }
    if (size == 8 || half) {
        c->msc = value;
    }
}

bool mace_audio_base_allowed(MACEAudio *s)
{
    for (unsigned i = 0; i < 3; i++) {
        if (!(s->channel[i].control & AUDIO_RESET)) {
            return false;
        }
    }
    return true;
}

void mace_audio_volume(MACEAudio *s, unsigned button, bool level)
{
    mace_audio_sync(s);
    if (level && !s->volume[button]) {
        s->control |= 1 << (23 + button);
    }
    s->volume[button] = level;
    audio_update_irq(s);
}

void mace_audio_set_input(MACEAudio *s, unsigned pin, int32_t value)
{
    assert(pin < 16);
    mace_audio_sync(s);
    if (pin < 14) {
        s->input[pin / 2][pin & 1] = value;
    } else {
        s->line_negative[pin & 1] = value;
    }
    codec_analog(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

void mace_audio_pdmnft(MACEAudio *s, bool level)
{
    mace_audio_sync(s);
    s->pdmnft = level;
    codec_analog(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

void mace_audio_pwrdwn(MACEAudio *s, bool level)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    mace_audio_sync(s);
    if (level == s->pwrdwn) {
        return;
    }
    s->pwrdwn = level;
    if (level) {
        s->pwrdwn_pending = true;
        if (s->power_state == POWER_ON) {
            s->power_state = POWER_STOPPING;
            s->reg[0] |= 0x4000;
            s->power_deadline = now + 5000000;
            codec_domains(s);
            codec_converters_update(s);
        } else if (s->power_state == POWER_DOWN) {
            codec_reset_state(s, now);
            s->pwrdwn_pending = false;
        }
    } else if (!(s->control & 1) && s->power_state == POWER_DOWN) {
        if (now - s->reset_start >= 100) {
            s->init_deadline = now + 600000;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-mace-audio: PWRDWN shorter than 100ns\n");
        }
    }
    audio_schedule(s);
}

static void audio_output(void *opaque, int available)
{
    MACEAudio *s = opaque;

    while (available >= 4 && s->host_count) {
        size_t pairs = MIN(s->host_count,
                            ARRAY_SIZE(s->host_output) / 2 - s->host_head);
        size_t accepted;

        pairs = MIN(pairs, available / 4);
        accepted = audio_be_write(s->backend, s->voice_out,
                                  &s->host_output[2 * s->host_head],
                                  pairs * 4) / 4;
        s->host_head = (s->host_head + accepted) %
                       (ARRAY_SIZE(s->host_output) / 2);
        s->host_count -= accepted;
        available -= accepted * 4;
        if (!accepted) {
            break;
        }
    }
}

static void audio_input(void *opaque, int available)
{
    MACEAudio *s = opaque;
    int16_t samples[128];

    mace_audio_sync(s);
    while (available >= 4) {
        size_t received = audio_be_read(s->backend, s->voice_in, samples,
                                        MIN(available, sizeof(samples)) & ~3);

        if (received < 4) {
            break;
        }
        /* AUX1 is the selected host input; conversions own guest progress. */
        s->input[MACE_AUDIO_AUX1][0] = samples[received / 2 - 2];
        s->input[MACE_AUDIO_AUX1][1] = samples[received / 2 - 1];
        available -= received;
    }
    codec_analog(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void audio_open_voices(MACEAudio *s)
{
    struct audsettings settings = {
        .freq = 48000,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = HOST_BIG_ENDIAN,
    };

    if (!s->backend) {
        return;
    }
    s->voice_out = audio_be_open_out(s->backend, s->voice_out,
                                    "MACE DAC1 line output", s, audio_output,
                                    &settings);
    s->voice_in = audio_be_open_in(s->backend, s->voice_in,
                                  "MACE AUX1 input", s, audio_input, &settings);
    if (s->voice_out) {
        audio_be_set_active_out(s->backend, s->voice_out, true);
    }
    if (s->voice_in) {
        audio_be_set_active_in(s->backend, s->voice_in, true);
    }
}

bool mace_audio_realize(MACEAudio *s, Error **errp)
{
    if (s->backend && !audio_be_check(&s->backend, errp)) {
        return false;
    }
    audio_open_voices(s);
    if (s->update_ttl) {
        s->update_ttl(s->opaque, (s->reg[28] >> 8) & 3);
    }
    return true;
}

void mace_audio_init(MACEAudio *s, AddressSpace *as, uint32_t *ring_base,
                     uint32_t (*get_ust)(void *, int64_t),
                     void (*update_irq)(void *, uint32_t), void *opaque)
{
    s->as = as;
    s->ring_base = ring_base;
    s->get_ust = get_ust;
    s->update_irq = update_irq;
    s->opaque = opaque;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, audio_timer, s);
    mace_audio_reset(s);
}

void mace_audio_reset(MACEAudio *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->control = 1;
    s->command = 0x10000;
    s->mask = 0;
    s->response = 0;
    s->pwrdwn = false;
    s->pwrdwn_pending = false;
    memset(s->channel, 0, sizeof(s->channel));
    for (unsigned i = 0; i < 3; i++) {
        s->channel[i].control = AUDIO_RESET;
    }
    codec_reset_state(s, now);
    timer_del(s->timer);
    audio_update_irq(s);
}

void mace_audio_cleanup(MACEAudio *s)
{
    timer_free(s->timer);
    s->timer = NULL;
    if (s->voice_out) {
        audio_be_close_out(s->backend, s->voice_out);
        s->voice_out = NULL;
    }
    if (s->voice_in) {
        audio_be_close_in(s->backend, s->voice_in);
        s->voice_in = NULL;
    }
}

static bool audio_load_double(QEMUFile *f, void *opaque, size_t size,
                               const VMStateField *field, Error **errp)
{
    uint64_t bits = qemu_get_be64(f);

    memcpy(opaque, &bits, sizeof(bits));
    return isfinite(*(double *)opaque);
}

static bool audio_save_double(QEMUFile *f, void *opaque, size_t size,
                               const VMStateField *field, JSONWriter *vmdesc,
                               Error **errp)
{
    uint64_t bits;

    memcpy(&bits, opaque, sizeof(bits));
    qemu_put_be64(f, bits);
    return true;
}

static const VMStateInfo vmstate_audio_double = {
    .name = "mace-audio-double",
    .load = audio_load_double,
    .save = audio_save_double,
};

#define VMSTATE_AUDIO_DOUBLE_ARRAY(field, state, count) \
    VMSTATE_ARRAY(field, state, count, 0, vmstate_audio_double, double)

static const VMStateDescription vmstate_audio_channel = {
    .name = "mace-audio/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(control, MACEAudioChannel),
        VMSTATE_UINT16(read, MACEAudioChannel),
        VMSTATE_UINT16(write, MACEAudioChannel),
        VMSTATE_UINT8_ARRAY(data, MACEAudioChannel, 32),
        VMSTATE_UINT8(count, MACEAudioChannel),
        VMSTATE_UINT8(cursor, MACEAudioChannel),
        VMSTATE_UINT8(fault, MACEAudioChannel),
        VMSTATE_UINT32(msc, MACEAudioChannel),
        VMSTATE_UINT32(ust, MACEAudioChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_audio_clock = {
    .name = "mace-audio/clock",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(time, MACEAudioClock),
        VMSTATE_INT64(progress, MACEAudioClock),
        VMSTATE_UINT64(rate, MACEAudioClock),
        VMSTATE_INT64(next, MACEAudioClock),
        VMSTATE_INT64(phase_next, MACEAudioClock),
        VMSTATE_UINT64(work_index, MACEAudioClock),
        VMSTATE_UINT16(phase, MACEAudioClock),
        VMSTATE_UINT16(phase_latch, MACEAudioClock),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_audio_converter = {
    .name = "mace-audio/converter",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(rate, MACEAudioConverter),
        VMSTATE_UINT8(edges, MACEAudioConverter),
        VMSTATE_BOOL(active, MACEAudioConverter),
        VMSTATE_INT16_ARRAY(fifo, MACEAudioConverter, 8),
        VMSTATE_UINT8(head, MACEAudioConverter),
        VMSTATE_UINT8(count, MACEAudioConverter),
        VMSTATE_BOOL(requested, MACEAudioConverter),
        VMSTATE_AUDIO_DOUBLE_ARRAY(previous, MACEAudioConverter, 2),
        VMSTATE_AUDIO_DOUBLE_ARRAY(sample, MACEAudioConverter, 2),
        VMSTATE_INT64(sample_time, MACEAudioConverter),
        VMSTATE_AUDIO_DOUBLE_ARRAY(output_previous, MACEAudioConverter, 2),
        VMSTATE_AUDIO_DOUBLE_ARRAY(output, MACEAudioConverter, 2),
        VMSTATE_INT64(output_time, MACEAudioConverter),
        VMSTATE_UINT64(converted, MACEAudioConverter),
        VMSTATE_UINT16(applied_gain, MACEAudioConverter),
        VMSTATE_UINT16(pending_gain, MACEAudioConverter),
        VMSTATE_UINT16(gain_wait, MACEAudioConverter),
        VMSTATE_END_OF_LIST()
    },
};

int mace_audio_post_load(MACEAudio *s)
{
    if ((s->control & ~0x1800001U) || (s->command & ~0xffffffU) ||
        s->serial_stage >= 6 || s->group > 1 ||
        (s->frame_slots != 16 && s->frame_slots != 32) ||
        (s->sclk != 12288000 && s->sclk != 16384000) ||
        s->serial_fraction >= s->sclk || s->power_state > POWER_STOPPING ||
        s->host_count > ARRAY_SIZE(s->host_output) / 2 ||
        s->host_head >= ARRAY_SIZE(s->host_output) / 2 ||
        s->fixture_policy != 0) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < 32; i++) {
        if (s->reg[i] & ~codec_mask[i]) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        MACEAudioChannel *c = &s->channel[i];

        if ((c->control & ~0x6e0) || (c->read & ~AUDIO_POINTER) ||
            (c->write & ~AUDIO_POINTER) || c->count > 4 || c->cursor > 4 ||
            c->fault > 3) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        MACEAudioConverter *c = &s->converter[i];
        MACEAudioClock *g = &s->clock[i];

        if (c->edges > 6 || c->count > 4 || c->head > 3 ||
            (c->active && !c->rate) || c->gain_wait > 384 ||
            (g->phase & ~0x1ff) || (g->phase_latch & ~0x1ff) ||
            g->progress >= CYCLE || g->progress < INT64_MIN / 4 ||
            g->rate > 65535 * 8 || c->rate != codec_rate(s->reg, i) ||
            (!g->rate && (g->next != NEVER || g->phase_next != NEVER)) ||
            (g->phase_next != NEVER && !(g->phase & 255)) ||
            (i == 3 && (g->phase || g->phase_next != NEVER ||
                         (g->rate && g->rate != FIXED_RATE)))) {
            return -EINVAL;
        }
    }
    audio_open_voices(s);
    audio_update_irq(s);
    if (s->update_ttl) {
        s->update_ttl(s->opaque, (s->reg[28] >> 8) & 3);
    }
    audio_schedule(s);
    return 0;
}

const VMStateDescription vmstate_mace_audio = {
    .name = "mace-audio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(channel, MACEAudio, 3, 0,
                              vmstate_audio_channel, MACEAudioChannel),
        VMSTATE_STRUCT_ARRAY(clock, MACEAudio, 4, 0,
                              vmstate_audio_clock, MACEAudioClock),
        VMSTATE_STRUCT_ARRAY(converter, MACEAudio, 4, 0,
                              vmstate_audio_converter, MACEAudioConverter),
        VMSTATE_UINT16_ARRAY(reg, MACEAudio, 32),
        VMSTATE_UINT32(control, MACEAudio),
        VMSTATE_UINT32(command, MACEAudio),
        VMSTATE_UINT16(mask, MACEAudio),
        VMSTATE_UINT16(response, MACEAudio),
        VMSTATE_UINT32(latched_command, MACEAudio),
        VMSTATE_UINT16(pending_response, MACEAudio),
        VMSTATE_UINT16(current_response, MACEAudio),
        VMSTATE_BOOL(response_pending, MACEAudio),
        VMSTATE_BOOL(transaction_valid, MACEAudio),
        VMSTATE_BOOL_ARRAY(playback_valid, MACEAudio, 2),
        VMSTATE_BOOL_ARRAY(capture_valid, MACEAudio, 2),
        VMSTATE_UINT16_ARRAY(capture_slot, MACEAudio, 2),
        VMSTATE_UINT8(group, MACEAudio),
        VMSTATE_UINT8(serial_stage, MACEAudio),
        VMSTATE_UINT8(frame_slots, MACEAudio),
        VMSTATE_UINT32(sclk, MACEAudio),
        VMSTATE_INT64(next_serial, MACEAudio),
        VMSTATE_UINT64(serial_fraction, MACEAudio),
        VMSTATE_INT64(reset_start, MACEAudio),
        VMSTATE_INT64(init_deadline, MACEAudio),
        VMSTATE_INT64(power_deadline, MACEAudio),
        VMSTATE_UINT8(power_state, MACEAudio),
        VMSTATE_BOOL(calibrated, MACEAudio),
        VMSTATE_BOOL(presence, MACEAudio),
        VMSTATE_BOOL(pwrdwn, MACEAudio),
        VMSTATE_BOOL(pwrdwn_pending, MACEAudio),
        VMSTATE_UINT64(frames, MACEAudio),
        VMSTATE_INT64(reference_ready, MACEAudio),
        VMSTATE_INT64(next_host, MACEAudio),
        VMSTATE_UINT64(host_fraction, MACEAudio),
        VMSTATE_2DARRAY(input, MACEAudio, MACE_AUDIO_INPUTS, 2, 0,
                        vmstate_audio_double, double),
        VMSTATE_AUDIO_DOUBLE_ARRAY(line_negative, MACEAudio, 2),
        VMSTATE_2DARRAY(line, MACEAudio, 2, 2, 0,
                        vmstate_audio_double, double),
        VMSTATE_AUDIO_DOUBLE_ARRAY(headphone, MACEAudio, 2),
        VMSTATE_SINGLE(mono, MACEAudio, 0, vmstate_audio_double, double),
        VMSTATE_BOOL(pdmnft, MACEAudio),
        VMSTATE_BOOL_ARRAY(volume, MACEAudio, 2),
        VMSTATE_UINT32_EQUAL(fixture_policy, MACEAudio),
        VMSTATE_INT16_ARRAY(host_output, MACEAudio, 4096),
        VMSTATE_UINT16(host_head, MACEAudio),
        VMSTATE_UINT16(host_count, MACEAudio),
        VMSTATE_END_OF_LIST()
    },
};
