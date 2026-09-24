/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI MediaIO RAD1: conversion between the audio channels' ring frames and
 * 16-bit stereo host frames.
 *
 * A ring frame is one 32-bit word per channel: two for the analog and AES3
 * channels, the first word the left channel, and eight for ADAT. Each word
 * holds a 24-bit signed sample in bits [23:0], sign-extended through
 * [31:24]. The words cross the Bridge's byte-lane reversal like the status
 * descriptors, so the device sees them little-endian. See
 * docs/datasheets/mediaio-rad1-audio.md, "Analog output sample format" and
 * the digital sections' flagged defaults; the left-first order, the
 * digital frame widths and the inputs' use of the same layout are flagged
 * defaults or the model's inference.
 *
 * Kept apart from sgi-rad1-audio.h, with no dependency beyond byte
 * access, so tests/unit/test-sgi-rad1-audio-pcm.c can pin the conversions
 * the model uses.
 */

#ifndef HW_AUDIO_SGI_RAD1_AUDIO_PCM_H
#define HW_AUDIO_SGI_RAD1_AUDIO_PCM_H

#include "qemu/bswap.h"

/* A 16-bit host sample sits in bits [23:8] of its 24-bit ring sample. */
#define SGI_RAD1_AUDIO_SAMPLE_SHIFT 8

/* Words a host frame carries: the mixer takes at most two channels. */
#define SGI_RAD1_AUDIO_PCM_CHANNELS 2

/* One ring word's 24-bit sample as a 16-bit host sample, bits [23:8]. */
static inline int16_t sgi_rad1_audio_word_to_sample(uint32_t word)
{
    return (int16_t)(word >> SGI_RAD1_AUDIO_SAMPLE_SHIFT);
}

/*
 * One 16-bit host sample as a ring word: bits [23:8], sign-extended
 * through [31:24], bits [7:0] zero.
 */
static inline uint32_t sgi_rad1_audio_sample_to_word(int16_t sample)
{
    return (uint32_t)((int32_t)sample * (1 << SGI_RAD1_AUDIO_SAMPLE_SHIFT));
}

/*
 * Output: @frames ring frames of @words_per_frame little-endian words each
 * to as many stereo 16-bit little-endian host frames. The first word of a
 * ring frame is left and the second right; any further words, ADAT
 * channels 3 to 8, have no host channel and are dropped.
 */
static inline void sgi_rad1_audio_frames_to_pcm(const uint8_t *ring,
                                                uint8_t *pcm, size_t frames,
                                                unsigned int words_per_frame)
{
    size_t f;
    unsigned int ch;

    for (f = 0; f < frames; f++) {
        for (ch = 0; ch < SGI_RAD1_AUDIO_PCM_CHANNELS; ch++) {
            uint32_t word = ldl_le_p(ring + (f * words_per_frame + ch) * 4);

            stw_le_p(pcm + (f * SGI_RAD1_AUDIO_PCM_CHANNELS + ch) * 2,
                     (uint16_t)sgi_rad1_audio_word_to_sample(word));
        }
    }
}

/*
 * Input: @frames stereo 16-bit little-endian host frames to as many ring
 * frames of @words_per_frame little-endian words. Left fills the first
 * word and right the second; any further words, ADAT channels 3 to 8,
 * record silence.
 */
static inline void sgi_rad1_audio_pcm_to_frames(const uint8_t *pcm,
                                                uint8_t *ring, size_t frames,
                                                unsigned int words_per_frame)
{
    size_t f;
    unsigned int w;

    for (f = 0; f < frames; f++) {
        for (w = 0; w < words_per_frame; w++) {
            uint32_t word = 0;

            if (w < SGI_RAD1_AUDIO_PCM_CHANNELS) {
                int16_t sample = (int16_t)lduw_le_p(
                    pcm + (f * SGI_RAD1_AUDIO_PCM_CHANNELS + w) * 2);

                word = sgi_rad1_audio_sample_to_word(sample);
            }
            stl_le_p(ring + (f * words_per_frame + w) * 4, word);
        }
    }
}

#endif
