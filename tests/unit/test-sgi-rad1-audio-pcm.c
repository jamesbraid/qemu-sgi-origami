/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SGI MediaIO RAD1 model's conversions between ring words and 16-bit
 * host samples (include/hw/audio/sgi-rad1-audio-pcm.h), pinned here
 * because no QEMU audio backend can feed a qtest known capture samples:
 * the only input a qtest can bind is silence, which every placement of a
 * sample turns into the same zero word.
 *
 * Each case puts a positive sample in one channel and a negative one in
 * the other, so a wrong bit position, a lost sign extension and swapped
 * channels each change the result.
 */

#include "qemu/osdep.h"
#include "hw/audio/sgi-rad1-audio-pcm.h"

/* One frame: the left word, then the right. */
static const uint32_t ring_frame[2] = { 0x00123456u, 0xffabcd12u };
/* What the output plays for it: bits [23:8] of each, left first. */
static const int16_t played[2] = { 0x1234, (int16_t)0xabcd };

static void test_words_to_pcm(void)
{
    uint8_t ring[8], pcm[4];

    stl_le_p(ring, ring_frame[0]);
    stl_le_p(ring + 4, ring_frame[1]);
    sgi_rad1_audio_words_to_pcm(ring, pcm, 2);

    g_assert_cmpint((int16_t)lduw_le_p(pcm), ==, played[0]);
    g_assert_cmpint((int16_t)lduw_le_p(pcm + 2), ==, played[1]);
}

static void test_pcm_to_words(void)
{
    static const struct {
        int16_t left, right;
        uint32_t left_word, right_word;
    } cases[] = {
        { 0x1234, (int16_t)0xabcd, 0x00123400u, 0xffabcd00u },
        { (int16_t)0x8000, 0x7fff, 0xff800000u, 0x007fff00u },
        { -1, 1, 0xffffff00u, 0x00000100u },
        { 0, 0, 0, 0 },
    };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        uint8_t pcm[4], ring[8];

        stw_le_p(pcm, (uint16_t)cases[i].left);
        stw_le_p(pcm + 2, (uint16_t)cases[i].right);
        sgi_rad1_audio_pcm_to_words(pcm, ring, 2);

        g_assert_cmphex((uint32_t)ldl_le_p(ring), ==, cases[i].left_word);
        g_assert_cmphex((uint32_t)ldl_le_p(ring + 4), ==,
                        cases[i].right_word);
    }
}

/* A recorded sample plays back unchanged. */
static void test_round_trip(void)
{
    uint8_t pcm[4], ring[8], back[4];

    stw_le_p(pcm, (uint16_t)played[0]);
    stw_le_p(pcm + 2, (uint16_t)played[1]);
    sgi_rad1_audio_pcm_to_words(pcm, ring, 2);
    sgi_rad1_audio_words_to_pcm(ring, back, 2);

    g_assert_cmpmem(back, sizeof(back), pcm, sizeof(pcm));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-rad1-audio/pcm/words-to-pcm", test_words_to_pcm);
    g_test_add_func("/sgi-rad1-audio/pcm/pcm-to-words", test_pcm_to_words);
    g_test_add_func("/sgi-rad1-audio/pcm/round-trip", test_round_trip);
    return g_test_run();
}
