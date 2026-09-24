/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI MediaIO RAD1: conversion between the analog channels' ring words and
 * 16-bit host samples.
 *
 * A ring frame is two 32-bit words, one channel each, the first word the
 * left channel; each word holds a 24-bit signed sample in bits [23:0],
 * sign-extended through [31:24]. The words cross the Bridge's byte-lane
 * reversal like the status descriptors, so the device sees them
 * little-endian. See docs/datasheets/mediaio-rad1-audio.md, "Analog output
 * sample format"; the left-first order is the datasheet's flagged default,
 * and the input's use of the same layout is the model's inference.
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

/*
 * Output: @words little-endian ring words to as many interleaved 16-bit
 * little-endian host samples, in ring order, each from bits [23:8].
 */
static inline void sgi_rad1_audio_words_to_pcm(const uint8_t *ring,
                                               uint8_t *pcm, size_t words)
{
    size_t i;

    for (i = 0; i < words; i++) {
        uint32_t word = ldl_le_p(ring + i * 4);

        stw_le_p(pcm + i * 2,
                 (uint16_t)(word >> SGI_RAD1_AUDIO_SAMPLE_SHIFT));
    }
}

/*
 * Input: @words interleaved 16-bit little-endian host samples to as many
 * little-endian ring words, in order, each sample in bits [23:8],
 * sign-extended through [31:24], bits [7:0] zero.
 */
static inline void sgi_rad1_audio_pcm_to_words(const uint8_t *pcm,
                                               uint8_t *ring, size_t words)
{
    size_t i;

    for (i = 0; i < words; i++) {
        int32_t sample = (int16_t)lduw_le_p(pcm + i * 2);

        stl_le_p(ring + i * 4,
                 (uint32_t)(sample * (1 << SGI_RAD1_AUDIO_SAMPLE_SHIFT)));
    }
}

#endif
